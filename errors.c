#include "errors.h"

#include <linux/printk.h>

struct pacct_errors pacct_errors = { 0 };

#define PACCT_ERROR_TRANSFORM(name)                                      \
	pr_info("  " #name " = %llu/%llu\n", pacct_errors.name.positive, \
		pacct_errors.name.positive + pacct_errors.name.negative);
void pacct_error_report(void)
{
	pr_info("pacct_errors {\n");
	PACCT_ERRORS
	pr_info("}\n");
}
#undef PACCT_ERROR_TRANSFORM

int pacct_error_trace(struct pacct_error_trace *tr, int cond)
{
	if (cond) {
		tr->positive++;
	} else {
		tr->negative++;
	}
	return cond;
}
