#ifndef _NCC_STDARG_H
#define _NCC_STDARG_H
typedef struct {
    unsigned int gp_offset;
    unsigned int fp_offset;
    void *overflow_arg_area;
    void *reg_save_area;
} va_list[1];
#define va_start(ap,last) __ncc_va_start(ap,last)
#define va_arg(ap,type) __ncc_va_arg(ap,type)
#define va_copy(destination,source) (*(destination)=*(source))
#define va_end(ap) ((void)(ap))
#endif
