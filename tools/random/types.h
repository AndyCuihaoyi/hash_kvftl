#ifndef FIO_TYPES_H
#define FIO_TYPES_H

#include <stdbool.h> /* IWYU pragma: export */

#if !defined(CONFIG_HAVE_KERNEL_RWF_T)
typedef int __kernel_rwf_t;
#endif

#endif