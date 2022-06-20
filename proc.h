#ifndef GTP5G_PROC_H__
#define GTP5G_PROC_H__

#include <linux/rculist.h>

extern int create_proc(void);
extern void remove_proc(void);

extern void init_proc_gtp5g_dev_list(void);
extern struct list_head * get_proc_gtp5g_dev_list_head(void);

#endif
