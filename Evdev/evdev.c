#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include "gwion_util.h"
#include "gwion_ast.h"
#include "gwion_env.h"
#include "vm.h"
#include "instr.h"
#include "gwion.h"
#include "object.h"
#include "plug.h"
#include "operator.h"
#include "import.h"
#include "shreduler_private.h"

#define EVDEV_PREFIX "/dev/input/event"

#define ABSINFO(o) *(struct input_absinfo**)(o->data + o_absinfo)
#define ABSINFO_CONST(o) *(m_int*)(o->data + o_absinfo_const)

#define VALUE(o) *(m_int*)(o->data + o_evdevev_value)
#define CODE(o) *(m_int*)(o->data + o_evdevev_code)
#define TYPE(o) *(m_int*)(o->data + o_evdevev_type)
#define SEC(o) *(m_int*)(o->data + o_evdevev_sec)
#define USEC(o) *(m_int*)(o->data + o_evdevev_usec)

#define INFO(o) *(EvdevInfo**)(o->data + o_evdev_info)
#define DELAY(o) *(m_int*)(o->data + o_evdev_delay)
#define PERIOD(o) *(m_int*)(o->data + o_evdev_period)

#define UINPUT(o) *(struct libevdev_uinput**)(o->data + o_uinput)

static Type t_absinfo;
m_int o_absinfo;
m_int o_absinfo_const;

static Type t_evdevev;
m_int o_evdevev_time;
m_int o_evdevev_type;
m_int o_evdevev_code;
m_int o_evdevev_value;
m_int o_evdevev_sec;
m_int o_evdevev_usec;

m_int o_evdev_info;
m_int o_evdev_delay;
m_int o_evdev_period;

m_int o_uinput;

static void* evdev_process(void* arg);

typedef struct {
  struct libevdev* evdev;
  int index;
  int fd;
  pthread_t thread;
  pthread_mutex_t mutex;
  MUTEX_TYPE bbq;
  Vector args;
  m_bool block;
} EvdevInfo;

static CTOR(absinfo_ctor) {
  ABSINFO(o) = (struct input_absinfo*)xcalloc(1, sizeof(struct input_absinfo));
}
static DTOR(absinfo_dtor) {
  if(!ABSINFO_CONST(o))
    free(ABSINFO(o));
}
#define describe_absinfo(name)                  \
static MFUN(absinfo_get_##name) {               \
  const struct input_absinfo* abs = ABSINFO(o); \
  const m_int i = *(m_int*)MEM(SZ_INT);         \
  *(m_int*)RETURN = abs->name;                  \
}                                               \
static MFUN(absinfo_set_##name) {               \
  struct input_absinfo* abs = ABSINFO(o);       \
  const m_int i = *(m_int*)MEM(SZ_INT);         \
  *(m_int*)RETURN = abs->name = i;              \
}
describe_absinfo(value)
describe_absinfo(minimum)
describe_absinfo(maximum)
describe_absinfo(fuzz)
describe_absinfo(flat)
describe_absinfo(resolution)
#define import_absinfo(name)                                    \
  GWI_BB(gwi_func_ini(gwi, "int", #name))\
  GWI_BB(gwi_func_end(gwi, absinfo_get_##name , ae_flag_none))                                \
  GWI_BB(gwi_func_ini(gwi, "int", #name)) \
  GWI_BB(gwi_func_arg(gwi, "int", "val"))                     \
  GWI_BB(gwi_func_end(gwi, absinfo_set_##name , ae_flag_none))

static MFUN(evdevev_is_type) {
  struct input_event ev;
  ev.type  = TYPE(o);
  ev.code   = CODE(o);
  ev.value = VALUE(o);
  ev.time.tv_sec   = SEC(o);
  ev.time.tv_usec  = USEC(o);
  const m_int type = *(m_int*)MEM(SZ_INT);
  *(m_int*)RETURN = libevdev_event_is_type(&ev, type);
}

static MFUN(evdevev_is_code) {
  struct input_event ev;
  ev.type  = TYPE(o);
  ev.code   = CODE(o);
  ev.value = VALUE(o);
  ev.time.tv_sec   = SEC(o);
  ev.time.tv_usec  = USEC(o);
  const m_int type = *(m_int*)MEM(SZ_INT);
  const m_int code = *(m_int*)MEM(SZ_INT*2);
  *(m_int*)RETURN = libevdev_event_is_type(&ev, code);
}

static CTOR(evdev_base_ctor) {
  EvdevInfo* info = INFO(o) = (EvdevInfo*)xcalloc(1, sizeof(EvdevInfo));
  info->evdev = libevdev_new();
  info->index = -1;
  info->args  = new_vector(shred->info->mp);
  info->bbq = shred->info->vm->shreduler->mutex;
}

static DTOR(evdev_dtor) {
  EvdevInfo* info = INFO(o);
  if(info->index != -1) {
    pthread_cancel(info->thread);
    pthread_join(info->thread, NULL);
    pthread_mutex_destroy(&info->mutex);
    close(info->fd);
  }
  libevdev_free(info->evdev);
  free_vector(shred->info->mp, info->args);
  free(info);
}

static void* evdev_process(void* arg) {
  int rc;
  struct input_event event;
  const M_Object o = (M_Object)arg;
  EvdevInfo* info = INFO(o);
  struct pollfd curr;

  curr.fd = info->fd;
  curr.events = POLL_IN;
  curr.revents = 0;
  const int flag = info->block ? LIBEVDEV_READ_FLAG_BLOCKING : LIBEVDEV_READ_FLAG_NORMAL;
  do {
    poll(&curr, 1, -1);
    rc = libevdev_next_event(info->evdev, flag, &event);
    if(rc == LIBEVDEV_READ_STATUS_SYNC) {
      do rc = libevdev_next_event(info->evdev, LIBEVDEV_READ_FLAG_FORCE_SYNC, &event);
      while(rc == LIBEVDEV_READ_STATUS_SYNC);
    }
    if(rc == LIBEVDEV_READ_STATUS_SUCCESS  && event.type) {
      pthread_mutex_lock(&info->mutex);
      struct input_event*ev  = (struct input_event*)xmalloc(sizeof(struct input_event));
      memcpy(ev, &event, sizeof(struct input_event));
      vector_add(info->args, (vtype)ev);
      broadcast(o);
      pthread_mutex_unlock(&info->mutex);
    }
  } while((rc == 1 || rc == 0 || rc == -11));
  return NULL;
}

static MFUN(evdev_index) {
  m_int index = *(m_int*)MEM(SZ_INT);
  EvdevInfo* info = INFO(o);
  char c[strlen(EVDEV_PREFIX) + num_digit(index) + 1];
  *(m_uint*)RETURN = -1;
  if(info->index != -1) {
    pthread_cancel(info->thread);
    pthread_join(info->thread, NULL);
    pthread_mutex_destroy(&info->mutex);
    close(libevdev_get_fd(info->evdev));
    libevdev_free(info->evdev);
    info->evdev = libevdev_new();
  }
  sprintf(c, "%s%i", EVDEV_PREFIX, index);
  if((info->fd = open((const char*)c,
      O_RDONLY | (info->block ? 0 : O_NONBLOCK))) == -1) {
    *(m_uint*)RETURN = -1;
    return;
  }
  libevdev_set_fd(info->evdev, info->fd);
  info->index = index;
  int delay, period;
  libevdev_get_repeat(info->evdev, &delay, &period);
  DELAY(o) = delay;
  PERIOD(o) = period;
  struct input_event event;
  while(libevdev_has_event_pending(info->evdev))
    libevdev_next_event(info->evdev, LIBEVDEV_READ_FLAG_NORMAL, &event);

  pthread_mutex_init(&info->mutex, NULL);
  pthread_create(&info->thread, NULL, evdev_process, o);
  *(m_uint*)RETURN = 1;
}

static MFUN(evdev_index_block) {
  EvdevInfo* info = INFO(o);
  info->block = *(m_int*)MEM(SZ_INT*2);
  evdev_index(o, RETURN, shred);
}

static MFUN(evdev_recv) {
  EvdevInfo* info = INFO(o);
  M_Object ev = *(M_Object*)MEM(SZ_INT);
  pthread_mutex_lock(&info->mutex);
  const Vector v = info->args;
  if(!vector_size(v)) {
    *(m_uint*)RETURN = 0;
    pthread_mutex_unlock(&info->mutex);
    return;
  }
  struct input_event* arg = (struct input_event*)vector_front(v);
  TYPE(ev)   = arg->type;
  CODE(ev)   = arg->code;
  VALUE(ev)  = arg->value;
  SEC(ev)  = arg->time.tv_sec;
  USEC(ev)  = arg->time.tv_usec;
  vector_rem(v, 0);
  free(arg);
  *(m_uint*)RETURN = 1;
  pthread_mutex_unlock(&info->mutex);
}

static MFUN(evdev_get_index) {
  const EvdevInfo* info = INFO(o);
  *(m_uint*)RETURN = info->index;
}

#define describe_var(func)                       \
static MFUN(evdev_##func) {                      \
  const EvdevInfo* info = INFO(o);               \
  const struct libevdev* dev = info->evdev;      \
  m_str str = (m_str)libevdev_get_##func(dev);   \
  *(M_Object*)RETURN  = new_string(shred->info->vm->gwion, str);  \
}                                                \
static MFUN(evdev_set_##func) {                  \
  const EvdevInfo* info = INFO(o);               \
  const M_Object obj = *(M_Object*)MEM(SZ_INT);  \
  if(!obj) {                                     \
    handle(shred, "NullPtrhandleion");           \
    return;                                      \
  }                                              \
  libevdev_set_##func(info->evdev, STRING(obj)); \
  *(M_Object*)RETURN  = obj;                     \
}

#define import_var(func)                                         \
  GWI_BB(gwi_func_ini(gwi, "string", #func))\
  GWI_BB(gwi_func_end(gwi, evdev_##func     , ae_flag_none))                                 \
  GWI_BB(gwi_func_ini(gwi, "string", #func))\
  GWI_BB(gwi_func_arg(gwi, "string", "str"))                   \
  GWI_BB(gwi_func_end(gwi, evdev_set_##func , ae_flag_none))
describe_var(name)
describe_var(phys)
describe_var(uniq)

static MFUN(evdev_version) {
  const EvdevInfo* info = INFO(o);
  *(m_int*)RETURN  = libevdev_get_driver_version(info->evdev);
}

static MFUN(evdev_get_num_slot) {
  const EvdevInfo* info = INFO(o);
  *(m_int*)RETURN  = libevdev_get_num_slots(info->evdev);
}

static MFUN(evdev_get_current_slot) {
  const EvdevInfo* info = INFO(o);
  *(m_int*)RETURN  = libevdev_get_current_slot(info->evdev);
}

static MFUN(evdev_grab) {
  const EvdevInfo* info = INFO(o);
  const m_int state = *(m_int*)MEM(SZ_INT);
  *(m_int*)RETURN  = libevdev_grab(info->evdev,
    state ? LIBEVDEV_GRAB : LIBEVDEV_UNGRAB);
}

static MFUN(evdev_led) {
  const EvdevInfo* info = INFO(o);
  const m_int code = *(m_int*)MEM(SZ_INT);
  const m_int state = *(m_int*)MEM(SZ_INT*2);
  *(m_int*)RETURN  = libevdev_kernel_set_led_value(info->evdev, code,
    state ? LIBEVDEV_GRAB : LIBEVDEV_UNGRAB);
}

static MFUN(evdev_clock) {
  const EvdevInfo* info = INFO(o);
  const m_int clock = *(m_int*)MEM(SZ_INT);
  *(m_int*)RETURN  = libevdev_set_clock_id(info->evdev, clock);
}

#define describe_get_set_fetch(func)                                              \
static MFUN(evdev_get_##func##_value) {                                           \
  const EvdevInfo* info = INFO(o);                                                \
  const m_int type = *(m_int*)MEM(SZ_INT);                                        \
  const m_int code = *(m_int*)MEM(SZ_INT*2);                                      \
  *(m_int*)RETURN  = libevdev_get_##func##_value(info->evdev, type, code);        \
}                                                                                 \
static MFUN(evdev_set_##func##_value) {                                           \
  const EvdevInfo* info = INFO(o);                                                \
  const m_int type  = *(m_int*)MEM(SZ_INT);                                       \
  const m_int code  = *(m_int*)MEM(SZ_INT*2);                                     \
  const m_int value = *(m_int*)MEM(SZ_INT*3);                                     \
  *(m_int*)RETURN  = libevdev_set_##func##_value(info->evdev, type, code, value); \
}                                                                                 \
static MFUN(evdev_fetch_##func##_value) {                                         \
  const EvdevInfo* info = INFO(o);                                                \
  const m_int type  = *(m_int*)MEM(SZ_INT);                                       \
  const m_int code  = *(m_int*)MEM(SZ_INT*2);                                     \
  int value = -1;                                                                 \
  libevdev_fetch_##func##_value(info->evdev, type, code, &value);                 \
  *(m_int*)RETURN  = value;                                                       \
}
describe_get_set_fetch(event)
describe_get_set_fetch(slot)
#define import_get_set_fetch(func)                                              \
  GWI_BB(gwi_func_ini(gwi, "void", "get_" #func))\
  GWI_BB(gwi_func_arg(gwi, "int", "type"))                                    \
  GWI_BB(gwi_func_arg(gwi, "int", "code"))                                    \
  GWI_BB(gwi_func_end(gwi, evdev_get_##func##_value   , ae_flag_none))                                                \
  GWI_BB(gwi_func_ini(gwi, "void", "set_" #func))\
  GWI_BB(gwi_func_arg(gwi, "int", "type"))                                    \
  GWI_BB(gwi_func_arg(gwi, "int", "code"))                                    \
  GWI_BB(gwi_func_arg(gwi, "int", "value"))                                   \
  GWI_BB(gwi_func_end(gwi, evdev_set_##func##_value   , ae_flag_none))                                                \
  GWI_BB(gwi_func_ini(gwi, "void", "fetch_" #func))\
  GWI_BB(gwi_func_arg(gwi, "int", "type"))                                    \
  GWI_BB(gwi_func_arg(gwi, "int", "code"))                                    \
  GWI_BB(gwi_func_end(gwi, evdev_get_##func##_value , ae_flag_none))                                                \

static MFUN(evdev_has_property) {
  const EvdevInfo* info = INFO(o);
  const m_int i = *(m_int*)MEM(SZ_INT);
  *(m_int*)RETURN  = libevdev_has_property(info->evdev, i);
}
static MFUN(evdev_enable_property) {
  const EvdevInfo* info = INFO(o);
  const m_int i = *(m_int*)MEM(SZ_INT);
  *(m_int*)RETURN  = libevdev_enable_property(info->evdev, i);
}
/*
static MFUN(evdev_disable_property) {
  const EvdevInfo* info = INFO(o);
  const m_int i = *(m_int*)MEM(SZ_INT);
  *(m_int*)RETURN  = libevdev_disable_property(info->evdev, i);
}
*/
static MFUN(evdev_has_event_type) {
  const EvdevInfo* info = INFO(o);
  const m_int i = *(m_int*)MEM(SZ_INT);
  *(m_int*)RETURN  = libevdev_has_event_type(info->evdev, i);
}
static MFUN(evdev_enable_event_type) {
  const EvdevInfo* info = INFO(o);
  const m_int i = *(m_int*)MEM(SZ_INT);
  *(m_int*)RETURN  = libevdev_enable_event_type(info->evdev, i);
}
static MFUN(evdev_disable_event_type) {
  const EvdevInfo* info = INFO(o);
  const m_int i = *(m_int*)MEM(SZ_INT);
  *(m_int*)RETURN  = libevdev_disable_event_type(info->evdev, i);
}
static MFUN(evdev_has_event_code) {
  const EvdevInfo* info = INFO(o);
  const m_int i = *(m_int*)MEM(SZ_INT);
  const m_int j = *(m_int*)MEM(SZ_INT*2);
  *(m_int*)RETURN  = libevdev_has_event_code(info->evdev, i, j);
}
static MFUN(evdev_enable_event_code) {
  const EvdevInfo* info = INFO(o);
  const m_int i = *(m_int*)MEM(SZ_INT);
  const m_int j = *(m_int*)MEM(SZ_INT*2);
  const M_Object k = *(M_Object*)MEM(SZ_INT*3);
  *(m_int*)RETURN  = libevdev_enable_event_code(info->evdev, i, j, k);
}
static MFUN(evdev_disable_event_code) {
  const EvdevInfo* info = INFO(o);
  const m_int i = *(m_int*)MEM(SZ_INT);
  const m_int j = *(m_int*)MEM(SZ_INT*2);
  *(m_int*)RETURN  = libevdev_disable_event_code(info->evdev, i, j);
}

#define describe_abs(name)                         \
static MFUN(evdev_get_abs_##name) {                \
  const EvdevInfo* info = INFO(o);                 \
  const m_int code = *(m_int*)MEM(SZ_INT);         \
  const m_int val = *(m_int*)MEM(SZ_INT*2);        \
  libevdev_get_abs_##name(info->evdev, code);      \
}                                                  \
static MFUN(evdev_set_abs_##name) {                \
  const EvdevInfo* info = INFO(o);                 \
  const m_int code = *(m_int*)MEM(SZ_INT);         \
  const m_int val = *(m_int*)MEM(SZ_INT*2);        \
  libevdev_set_abs_##name(info->evdev, code, val); \
}
describe_abs(minimum)
describe_abs(maximum)
describe_abs(fuzz)
describe_abs(flat)
describe_abs(resolution)
static MFUN(evdev_get_abs_info) {
  const EvdevInfo* info = INFO(o);
  const m_int code = *(m_int*)MEM(SZ_INT);
  if(!info->fd == -1) {
    handle(shred, "InvalidEvdevRequest");
    return;
  }
  const struct input_absinfo* abs = libevdev_get_abs_info(info->evdev, code);
  if(abs) {
    M_Object obj = new_object(shred->info->mp, t_absinfo);
    ABSINFO(obj) = (struct input_absinfo*)abs;
    ABSINFO_CONST(obj) = 1;
    *(M_Object*)RETURN = obj;
  } else
    handle(shred, "InvalidEvdevInfoRequest");
}

#define import_abs(name)                                                  \
  GWI_BB(gwi_func_ini(gwi, "void", "abs_" #name))\
  GWI_BB(gwi_func_arg(gwi, "int", "code"))                              \
  GWI_BB(gwi_func_end(gwi, evdev_get_abs_##name , ae_flag_none))                                          \
  GWI_BB(gwi_func_ini(gwi, "void", "abs_" #name))\
  GWI_BB(gwi_func_arg(gwi, "int", "code"))                              \
  GWI_BB(gwi_func_arg(gwi, "int", "val"))                               \
  GWI_BB(gwi_func_end(gwi, evdev_set_abs_##name , ae_flag_none))                                          \

static MFUN(evdev_set_abs_info) {
  const EvdevInfo* info = INFO(o);
  const m_int code = *(m_int*)MEM(SZ_INT);
  const M_Object obj = *(M_Object*)MEM(SZ_INT*2);
  const struct input_absinfo* abs = ABSINFO(obj);
  libevdev_set_abs_info(info->evdev, code, abs);
}
static MFUN(evdev_kernel_set_abs_info) {
  const EvdevInfo* info = INFO(o);
  const m_int code = *(m_int*)MEM(SZ_INT);
  const M_Object obj = *(M_Object*)MEM(SZ_INT*2);
  const struct input_absinfo* abs = ABSINFO(obj);
  *(m_int*)RETURN = libevdev_kernel_set_abs_info(info->evdev, code, abs);
}
#define import_set_absinfo(type, func)                                          \
  GWI_BB(gwi_func_ini(gwi, #type, #func "absinfo"))\
  GWI_BB(gwi_func_arg(gwi, "int", "code"))                                    \
  GWI_BB(gwi_func_arg(gwi, "AbsInfo", "abs"))                                 \
  GWI_BB(gwi_func_end(gwi, evdev_##func##set_abs_info , ae_flag_none))                                                \

#define describe_id(name)                                \
static MFUN(evdev_get_id_##name) {                       \
  const EvdevInfo* info = INFO(o);                       \
  *(m_int*)RETURN = libevdev_get_id_##name(info->evdev); \
}                                                        \
static MFUN(evdev_set_id_##name) {                       \
  const EvdevInfo* info = INFO(o);                       \
  const m_int id = *(m_int*)MEM(SZ_INT);                 \
  libevdev_set_id_##name(info->evdev, id);               \
  *(m_int*)RETURN = id;                                  \
}
describe_id(product)
describe_id(vendor)
describe_id(bustype)
describe_id(version)
#define import_id(name)                                         \
  GWI_BB(gwi_func_ini(gwi, "int", #name))\
  GWI_BB(gwi_func_end(gwi, evdev_get_id_##name , ae_flag_none))                                 \
  GWI_BB(gwi_func_ini(gwi, "int", #name))\
  GWI_BB(gwi_func_arg(gwi, "int", "id"))                       \
  GWI_BB(gwi_func_end(gwi, evdev_set_id_##name , ae_flag_none))                                 \

#define describe_from_name(func)                                \
static MFUN(evdev_##func##_from_name) {                       \
  const EvdevInfo* info = INFO(o);                       \
  const M_Object str = *(M_Object*)MEM(SZ_INT);                 \
  if(!str) {handle(shred, "NullPtrhandleion"); return; }\
  *(m_int*)RETURN = libevdev_##func##_from_name(STRING(str)); \
}                                                        \
static MFUN(evdev_##func##_from_name_n) {                       \
  const EvdevInfo* info = INFO(o);                       \
  const M_Object str = *(M_Object*)MEM(SZ_INT);                 \
  if(!str) {handle(shred, "NullPtrhandleion"); return; }\
  const m_int n  = *(m_int*)MEM(SZ_INT*2);                 \
  *(m_int*)RETURN = libevdev_##func##_from_name_n(STRING(str), n); \
}
describe_from_name(event_type)
//describe_from_name(event_code)
describe_from_name(property)
#define import_from_name(func)                                         \
  GWI_BB(gwi_func_ini(gwi, "int", #func))\
  GWI_BB(gwi_func_arg(gwi, "string", "name"))                       \
  GWI_BB(gwi_func_end(gwi, evdev_##func##_from_name , ae_flag_static))                                 \
  GWI_BB(gwi_func_ini(gwi, "int", #func))\
  GWI_BB(gwi_func_arg(gwi, "string", "name"))                       \
  GWI_BB(gwi_func_arg(gwi, "int", "n"))                       \
  GWI_BB(gwi_func_end(gwi, evdev_##func##_from_name_n , ae_flag_static))                                 \

static MFUN(evdev_event_code_from_name) {
  const EvdevInfo* info = INFO(o);
  const m_int type = *(m_int*)MEM(SZ_INT);
  const M_Object str = *(M_Object*)MEM(SZ_INT*2);
  if(!str) {handle(shred, "NullPtrhandleion");return;}
  *(m_int*)RETURN = libevdev_event_code_from_name(type, STRING(str));
}
static MFUN(evdev_eventcode_from_name_n) {
  const EvdevInfo* info = INFO(o);
  const m_int type = *(m_int*)MEM(SZ_INT);
  const M_Object str = *(M_Object*)MEM(SZ_INT);
  if(!str){ handle(shred, "NullPtrhandleion"); return; }
  const m_int n  = *(m_int*)MEM(SZ_INT*2);
  *(m_int*)RETURN = libevdev_event_code_from_name_n(type, STRING(str), n);
}

static DTOR(uinput_dtor) {
  struct libevdev_uinput *uidev = UINPUT(o);
  if(uidev) {
    const int fd = libevdev_uinput_get_fd(uidev);
    libevdev_uinput_destroy(uidev);
    close(fd);
  }
}

static MFUN(uinput_fd) {
  struct libevdev_uinput* uidev = UINPUT(o);
  *(m_int*)RETURN = uidev ? libevdev_uinput_get_fd(uidev): -1;
}
static MFUN(uinput_create) {
  const M_Object ev = *(M_Object*)MEM(SZ_INT);
  if(!ev) {
    handle(shred, "NullPtrhandleion");
    return;
  }
  const EvdevInfo* info = INFO(ev);
  if(info->fd == -1) {
    *(m_int*)RETURN = -1;
    return;
  }
  const struct libevdev* dev = info->evdev;
  const int uifd = open("/dev/uinput", O_RDWR);
  *(m_int*)RETURN = libevdev_uinput_create_from_device(dev, uifd, &UINPUT(o));
}

#define describe_uinput(func)                                                \
static MFUN(uinput_##func) {                                                 \
  struct libevdev_uinput* uidev = UINPUT(o);                                 \
  *(M_Object*)RETURN = uidev ?                                               \
    new_string(shred->info->vm->gwion, (const m_str)libevdev_uinput_get_##func(uidev)): NULL; \
}
describe_uinput(syspath)
describe_uinput(devnode)
static MFUN(uinput_write) {
  struct libevdev_uinput* uidev = UINPUT(o);
  const m_int type  = *(m_int*)MEM(SZ_INT);
  const m_int code  = *(m_int*)MEM(SZ_INT);
  const m_int value = *(m_int*)MEM(SZ_INT*2);
  *(m_int*)RETURN = uidev ?
    libevdev_uinput_write_event(uidev, type, code, value): -1;
}

GWION_IMPORT(evdev) {
  GWI_BB(gwi_class_ini(gwi, "EvdevEv", NULL))
  GWI_BB(gwi_item_ini(gwi, "int", "type"))
  GWI_BB((o_evdevev_type  = gwi_item_end(gwi, ae_flag_const, num, 0)))

  GWI_BB(gwi_item_ini(gwi, "int", "code"))
  GWI_BB((o_evdevev_code  = gwi_item_end(gwi, ae_flag_const, num, 0)))

  GWI_BB(gwi_item_ini(gwi, "int", "value"))
  GWI_BB((o_evdevev_value  = gwi_item_end(gwi, ae_flag_const, num, 0)))

  GWI_BB(gwi_item_ini(gwi, "int", "sec"))
  GWI_BB((o_evdevev_sec  = gwi_item_end(gwi, ae_flag_const, num, 0)))

  GWI_BB(gwi_item_ini(gwi, "int", "usec"))
  GWI_BB((o_evdevev_usec  = gwi_item_end(gwi, ae_flag_const, num, 0)))

  GWI_BB(gwi_func_ini(gwi, "int", "is_type"))
  GWI_BB(gwi_func_arg(gwi, "int", "type"))
  GWI_BB(gwi_func_end(gwi, evdevev_is_type, ae_flag_none))

  GWI_BB(gwi_func_ini(gwi, "int", "is_code"))
  GWI_BB(gwi_func_arg(gwi, "int", "type"))
  GWI_BB(gwi_func_arg(gwi, "int", "code"))
  GWI_BB(gwi_func_end(gwi, evdevev_is_code, ae_flag_none))

  GWI_BB(gwi_class_end(gwi))

  GWI_BB(gwi_class_ini(gwi, "AbsInfo", NULL))
  gwi_class_xtor(gwi, absinfo_ctor, absinfo_dtor);
  GWI_BB(gwi_item_ini(gwi, "@internal", "@info"))
  GWI_BB((o_absinfo = gwi_item_end(gwi, ae_flag_const, num, 0)))
  GWI_BB(gwi_item_ini(gwi, "int", "const"))
  GWI_BB((o_absinfo_const = gwi_item_end(gwi, ae_flag_const, num, 0)))
  import_absinfo(value)
  import_absinfo(minimum)
  import_absinfo(maximum)
  import_absinfo(fuzz)
  import_absinfo(flat)
  import_absinfo(resolution)
  GWI_BB(gwi_class_end(gwi))

  GWI_BB(gwi_class_ini(gwi, "Evdev", "Event"))
  gwi_class_xtor(gwi, evdev_base_ctor, evdev_dtor);

  GWI_BB(gwi_item_ini(gwi, "@internal", "@info"))
  GWI_BB((o_evdev_info = gwi_item_end(gwi, ae_flag_const, num, 0)))

  GWI_BB(gwi_item_ini(gwi, "int", "repeat_delay"))
  GWI_BB((o_evdev_delay  = gwi_item_end(gwi, ae_flag_const, num, 0)))

  GWI_BB(gwi_item_ini(gwi, "int", "repeat_period"))
  GWI_BB((o_evdev_period  = gwi_item_end(gwi, ae_flag_const, num, 0)))

  GWI_BB(gwi_func_ini(gwi, "int", "index"))
  GWI_BB(gwi_func_arg(gwi, "int", "i"))
  GWI_BB(gwi_func_end(gwi, evdev_index, ae_flag_none))

  GWI_BB(gwi_func_ini(gwi, "int", "index"))
  GWI_BB(gwi_func_end(gwi, evdev_get_index, ae_flag_none))

  GWI_BB(gwi_func_ini(gwi, "int", "num_slots"))
  GWI_BB(gwi_func_end(gwi, evdev_get_num_slot, ae_flag_none))

  GWI_BB(gwi_func_ini(gwi, "int", "curent_slot"))
  GWI_BB(gwi_func_end(gwi, evdev_get_current_slot, ae_flag_none))

  GWI_BB(gwi_func_ini(gwi, "int", "index"))
  GWI_BB(gwi_func_arg(gwi, "int", "i"))
  GWI_BB(gwi_func_arg(gwi, "int", "block"))
  GWI_BB(gwi_func_end(gwi, evdev_index_block, ae_flag_none))

  import_var(name)
  import_var(phys)
  import_var(uniq)

  GWI_BB(gwi_func_ini(gwi, "int", "recv"))
  GWI_BB(gwi_func_arg(gwi, "EvdevEv", "ev"))
  GWI_BB(gwi_func_end(gwi, evdev_recv, ae_flag_none))

  GWI_BB(gwi_func_ini(gwi, "int", "version"))
  GWI_BB(gwi_func_end(gwi, evdev_version, ae_flag_none))

  GWI_BB(gwi_func_ini(gwi, "int", "grab"))
  GWI_BB(gwi_func_arg(gwi, "int", "i"))
  GWI_BB(gwi_func_end(gwi, evdev_grab, ae_flag_none))

  GWI_BB(gwi_func_ini(gwi, "int", "clock"))
  GWI_BB(gwi_func_arg(gwi, "int", "i"))
  GWI_BB(gwi_func_end(gwi, evdev_clock, ae_flag_none))

  GWI_BB(gwi_func_ini(gwi, "int", "led"))
  GWI_BB(gwi_func_arg(gwi, "int", "code"))
  GWI_BB(gwi_func_arg(gwi, "int", "state"))
  GWI_BB(gwi_func_end(gwi, evdev_led, ae_flag_none))

  import_abs(minimum)
  import_abs(maximum)
  import_abs(fuzz)
  import_abs(flat)
  import_abs(resolution)
  import_set_absinfo(void,)
  import_set_absinfo(int, kernel_)
  GWI_BB(gwi_func_ini(gwi, "AbsInfo", "get_absinfo"))
  GWI_BB(gwi_func_arg(gwi, "int", "code"))
  GWI_BB(gwi_func_end(gwi, evdev_get_abs_info, ae_flag_none))

  import_id(product)
  import_id(vendor)
  import_id(bustype)
  import_id(version)

  import_from_name(event_type)
//  import_from_name(event_code)
  import_from_name(property)
  GWI_BB(gwi_func_ini(gwi, "int", "event_code"))
  GWI_BB(gwi_func_arg(gwi, "int", "type"))
  GWI_BB(gwi_func_arg(gwi, "string", "name"))
  GWI_BB(gwi_func_end(gwi, evdev_event_code_from_name, ae_flag_static))
  GWI_BB(gwi_func_ini(gwi, "int", "event_code"))
  GWI_BB(gwi_func_arg(gwi, "string", "name"))
  GWI_BB(gwi_func_arg(gwi, "int", "n"))
  GWI_BB(gwi_func_end(gwi, evdev_eventcode_from_name_n, ae_flag_static))

  GWI_BB(gwi_func_ini(gwi, "int", "has_property"))
  GWI_BB(gwi_func_arg(gwi, "int", "i"))
  GWI_BB(gwi_func_end(gwi, evdev_has_property, ae_flag_none))
  GWI_BB(gwi_func_ini(gwi, "int", "enable_property"))
  GWI_BB(gwi_func_arg(gwi, "int", "i"))
  GWI_BB(gwi_func_end(gwi, evdev_enable_property, ae_flag_none))
/*
  GWI_BB(gwi_func_ini(gwi, "int", "disable_property")
  GWI_BB(gwi_func_arg(gwi, "int", "i"))
  GWI_BB(gwi_func_end(gwi, evdev_disable_property, ae_flag_none))
*/
  GWI_BB(gwi_func_ini(gwi, "int", "has_event_type"))
  GWI_BB(gwi_func_arg(gwi, "int", "i"))
  GWI_BB(gwi_func_end(gwi, evdev_has_event_type, ae_flag_none))
  GWI_BB(gwi_func_ini(gwi, "int", "enable_event_type"))
  GWI_BB(gwi_func_arg(gwi, "int", "i"))
  GWI_BB(gwi_func_end(gwi, evdev_enable_event_type, ae_flag_none))
  GWI_BB(gwi_func_ini(gwi, "int", "disable_event_type"))
  GWI_BB(gwi_func_arg(gwi, "int", "i"))
  GWI_BB(gwi_func_end(gwi, evdev_disable_event_type, ae_flag_none))
  GWI_BB(gwi_func_ini(gwi, "int", "has_event_code"))
  GWI_BB(gwi_func_arg(gwi, "int", "i"))
  GWI_BB(gwi_func_arg(gwi, "int", "code"))
  GWI_BB(gwi_func_end(gwi, evdev_has_event_code, ae_flag_none))
  GWI_BB(gwi_func_ini(gwi, "int", "enable_event_code"))
  GWI_BB(gwi_func_arg(gwi, "int", "i"))
  GWI_BB(gwi_func_arg(gwi, "int", "code"))
  GWI_BB(gwi_func_arg(gwi, "Object", "opt"))
  GWI_BB(gwi_func_end(gwi, evdev_enable_event_code, ae_flag_none))
  GWI_BB(gwi_func_ini(gwi, "int", "disable_event_code"))
  GWI_BB(gwi_func_arg(gwi, "int", "i"))
  GWI_BB(gwi_func_arg(gwi, "int", "code"))
  GWI_BB(gwi_func_end(gwi, evdev_disable_event_code, ae_flag_none))

  import_get_set_fetch(event)
  import_get_set_fetch(slot)

  GWI_BB(gwi_class_end(gwi))

  GWI_BB(gwi_class_ini(gwi, "Uinput", NULL))
  gwi_class_xtor(gwi, NULL, uinput_dtor);
  GWI_BB(gwi_item_ini(gwi, "@internal", "@uinput"))
  GWI_BB((o_uinput = gwi_item_end(gwi, ae_flag_const, num, 0)))
  GWI_BB(gwi_func_ini(gwi, "int", "create"))
  GWI_BB(gwi_func_arg(gwi, "Evdev", "ev"))
  GWI_BB(gwi_func_end(gwi, uinput_create, ae_flag_none))
  GWI_BB(gwi_func_ini(gwi, "int", "fd"))
  GWI_BB(gwi_func_end(gwi, uinput_fd, ae_flag_none))
  GWI_BB(gwi_func_ini(gwi, "string", "syspath"))
  GWI_BB(gwi_func_end(gwi, uinput_syspath, ae_flag_none))
  GWI_BB(gwi_func_ini(gwi, "string", "devnode"))
  GWI_BB(gwi_func_end(gwi, uinput_devnode, ae_flag_none))
  GWI_BB(gwi_func_ini(gwi, "string", "write"))
  GWI_BB(gwi_func_arg(gwi, "int", "type"))
  GWI_BB(gwi_func_arg(gwi, "int", "code"))
  GWI_BB(gwi_func_arg(gwi, "int", "value"))
  GWI_BB(gwi_func_end(gwi, uinput_write, ae_flag_none))
  GWI_BB(gwi_class_end(gwi))

  return GW_OK;
}
