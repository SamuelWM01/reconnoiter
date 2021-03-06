/*
 * Copyright (c) 2007, OmniTI Computer Consulting, Inc.
 * All rights reserved.
 * Copyright (c) 2015-2017, Circonus, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name OmniTI Computer Consulting, Inc. nor the names
 *       of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written
 *       permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _NOIT_FILTERS_H
#define _NOIT_FILTERS_H

#include <mtev_defines.h>
#include <mtev_hash.h>
#include <mtev_console.h>
#include <mtev_conf.h>
#include "noit_check.h"

API_EXPORT(void)
  noit_filters_init();

API_EXPORT(void)
  noit_refresh_filtersets();

API_EXPORT(mtev_boolean)
  noit_apply_filterset(const char *filterset,
                       noit_check_t *check,
                       metric_t *metric);

API_EXPORT(void)
  noit_filter_compile_add(mtev_conf_section_t setinfo);

API_EXPORT(int)
  noit_filter_remove(mtev_conf_section_t setinfo);

API_EXPORT(int)
  noit_filter_exists(const char *name);

API_EXPORT(int)
  noit_filter_get_seq(const char *name, uint64_t *seq);

API_EXPORT(void)
  noit_filters_rest_init();

API_EXPORT(int)
  noit_filtersets_cull_unused();

API_EXPORT(void)
  noit_filters_init_globals(void);

#endif
