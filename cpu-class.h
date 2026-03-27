#ifndef PACCT_CPU_CLASS_H
#define PACCT_CPU_CLASS_H

enum pacct_cpu_class {
	PACCT_CPU_CLASS_EFFICIENCY = 0,
	PACCT_CPU_CLASS_PERFORMANCE,
};

int pacct_cpu_class_init(void);
enum pacct_cpu_class pacct_cpu_class_get(int cpu);

#endif
