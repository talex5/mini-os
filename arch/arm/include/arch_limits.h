#ifndef __ARCH_LIMITS_H__
#define __ARCH_LIMITS_H__

#define __PAGE_SHIFT        12
#define __PAGE_SIZE        (1 << __PAGE_SHIFT)

#define __STACK_SIZE_PAGE_ORDER  2
#define __STACK_SIZE (4 * __PAGE_SIZE)

#endif
