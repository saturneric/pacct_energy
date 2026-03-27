#ifndef PACCT_PROC_H
#define PACCT_PROC_H

#include "traced-task.h"

int pacct_proc_init(void);
void pacct_proc_remove(void);
int pacct_proc_file_setup(struct traced_task *entry);
void pacct_proc_file_free(struct traced_task *entry);

#endif
