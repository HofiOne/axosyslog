/*
 * Copyright (c) 2023 Balazs Scheidler <balazs.scheidler@axoflow.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */
#include <criterion/criterion.h>
#include "libtest/filterx-lib.h"

#include "filterx/object-json.h"
#include "filterx/object-string.h"
#include "filterx/object-message-value.h"
#include "filterx/expr-function.h"
#include "apphook.h"
#include "scratch-buffers.h"

Test(filterx_json, test_filterx_object_json_from_repr)
{
  FilterXObject *fobj;

  fobj = filterx_json_new_from_repr("{\"foo\": \"foovalue\"}", -1);
  cr_assert(filterx_object_is_type(fobj, &FILTERX_TYPE_NAME(json_object)));
  assert_object_json_equals(fobj, "{\"foo\":\"foovalue\"}");
  assert_marshaled_object(fobj, "{\"foo\":\"foovalue\"}", LM_VT_JSON);
  filterx_object_unref(fobj);

  fobj = filterx_json_new_from_repr("[\"foo\", \"bar\"]", -1);
  cr_assert(filterx_object_is_type(fobj, &FILTERX_TYPE_NAME(json_array)));
  assert_object_json_equals(fobj, "[\"foo\",\"bar\"]");
  assert_marshaled_object(fobj, "foo,bar", LM_VT_LIST);
  filterx_object_unref(fobj);

  fobj = filterx_json_new_from_repr("[1, 2]", -1);
  cr_assert(filterx_object_is_type(fobj, &FILTERX_TYPE_NAME(json_array)));
  assert_object_json_equals(fobj, "[1,2]");
  assert_marshaled_object(fobj, "[1,2]", LM_VT_JSON);
  filterx_object_unref(fobj);

  fobj = filterx_json_array_new_from_syslog_ng_list("\"foo\",bar", -1);
  cr_assert(filterx_object_is_type(fobj, &FILTERX_TYPE_NAME(json_array)));
  assert_object_json_equals(fobj, "[\"foo\",\"bar\"]");
  assert_marshaled_object(fobj, "foo,bar", LM_VT_LIST);
  filterx_object_unref(fobj);

  fobj = filterx_json_array_new_from_syslog_ng_list("1,2", -1);
  cr_assert(filterx_object_is_type(fobj, &FILTERX_TYPE_NAME(json_array)));
  assert_object_json_equals(fobj, "[\"1\",\"2\"]");
  assert_marshaled_object(fobj, "1,2", LM_VT_LIST);
  filterx_object_unref(fobj);
}

static FilterXObject *
_exec_func(FilterXSimpleFunctionProto func, FilterXObject *arg)
{
  if (!arg)
    return func(NULL, NULL, 0);

  FilterXObject *args[] = { arg };
  FilterXObject *result = func(NULL, args, G_N_ELEMENTS(args));
  filterx_object_unref(arg);
  return result;
}

static FilterXObject *
_exec_json_func(FilterXObject *arg)
{
  return _exec_func(filterx_json_new_from_args, arg);
}

static FilterXObject *
_exec_json_array_func(FilterXObject *arg)
{
  return _exec_func(filterx_json_array_new_from_args, arg);
}

Test(filterx_json, test_json_function)
{
  FilterXObject *fobj;

  fobj = _exec_json_func(NULL);
  cr_assert(filterx_object_is_type(fobj, &FILTERX_TYPE_NAME(json_object)));
  assert_object_json_equals(fobj, "{}");
  filterx_object_unref(fobj);

  fobj = _exec_json_func(filterx_string_new("{\"foo\": 1}", -1));
  cr_assert(filterx_object_is_type(fobj, &FILTERX_TYPE_NAME(json_object)));
  assert_object_json_equals(fobj, "{\"foo\":1}");
  filterx_object_unref(fobj);

  /* message_value is being unmarshalled before passing it to a simple
   * function, so no need to handle FilterXMessageValue instances */
  fobj = _exec_json_func(filterx_message_value_new("{\"foo\": 1}", -1, LM_VT_JSON));
  cr_assert(fobj == NULL);

  fobj = _exec_json_func(filterx_string_new("[1, 2]", -1));
  cr_assert(filterx_object_is_type(fobj, &FILTERX_TYPE_NAME(json_array)));
  assert_object_json_equals(fobj, "[1,2]");
  filterx_object_unref(fobj);

  fobj = _exec_json_func(filterx_json_object_new_from_repr("{\"foo\": 1}", -1));
  cr_assert(filterx_object_is_type(fobj, &FILTERX_TYPE_NAME(json_object)));
  assert_object_json_equals(fobj, "{\"foo\":1}");
  filterx_object_unref(fobj);

  fobj = _exec_json_func(filterx_json_array_new_from_repr("[1, 2]", -1));
  cr_assert(filterx_object_is_type(fobj, &FILTERX_TYPE_NAME(json_array)));
  assert_object_json_equals(fobj, "[1,2]");
  filterx_object_unref(fobj);
}

Test(filterx_json, test_json_array_function)
{
  FilterXObject *fobj;

  fobj = _exec_json_array_func(NULL);
  cr_assert(filterx_object_is_type(fobj, &FILTERX_TYPE_NAME(json_array)));
  assert_object_json_equals(fobj, "[]");
  filterx_object_unref(fobj);

  fobj = _exec_json_array_func(filterx_string_new("[1, 2]", -1));
  cr_assert(filterx_object_is_type(fobj, &FILTERX_TYPE_NAME(json_array)));
  assert_object_json_equals(fobj, "[1,2]");
  filterx_object_unref(fobj);

  /* message_value is being unmarshalled before passing it to a simple
   * function, so no need to handle FilterXMessageValue instances */
  fobj = _exec_json_array_func(filterx_message_value_new("[1, 2]", -1, LM_VT_JSON));
  cr_assert(fobj == NULL);

  fobj = _exec_json_array_func(filterx_json_array_new_from_repr("[1, 2]", -1));
  cr_assert(filterx_object_is_type(fobj, &FILTERX_TYPE_NAME(json_array)));
  assert_object_json_equals(fobj, "[1,2]");
  filterx_object_unref(fobj);
}

Test(filterx_json, filterx_json_object_repr)
{
  FilterXObject *obj = filterx_json_object_new_from_repr("{\"foo\": \"foovalue\"}", -1);
  assert_object_repr_equals(obj, "{\"foo\":\"foovalue\"}");
  filterx_object_unref(obj);
}

Test(filterx_json, filterx_json_array_repr)
{
  FilterXObject *obj = filterx_json_array_new_from_repr("[\"foo\", \"bar\"]", -1);
  assert_object_repr_equals(obj, "[\"foo\",\"bar\"]");
  filterx_object_unref(obj);
}

Test(filterx_json, filterx_json_object_cloning_double)
{
  FilterXObject *obj = filterx_json_object_new_from_repr("{\"foo\": 3.14}", -1);
  FilterXObject *obj_clone = filterx_object_clone(obj);
  cr_assert_not_null(obj_clone);
  filterx_object_unref(obj_clone);
  filterx_object_unref(obj);
}

static void
setup(void)
{
  app_startup();
  init_libtest_filterx();
}

static void
teardown(void)
{
  scratch_buffers_explicit_gc();
  deinit_libtest_filterx();
  app_shutdown();
}

TestSuite(filterx_json, .init = setup, .fini = teardown);
