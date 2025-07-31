#ifndef __DFTL_WB_H__
#define __DFTL_WB_H__

#include "write_buffer.h"

extern w_buffer_t write_buffer;

void dftl_wb_init(w_buffer_t *self);
void dftl_wb_destroy(w_buffer_t *self);
bool wb_is_full(w_buffer_t *self);
uint32_t do_wb_check(w_buffer_t *self, request *const req);
uint32_t _do_wb_insert(w_buffer_t *self, request *const req);
void _do_wb_assign_ppa(w_buffer_t *self, request *req);
void _do_wb_mapping_update(w_buffer_t *self, request *req);
void _do_wb_flush(w_buffer_t *self, request *req);

#endif // __DFTL_WB_H__