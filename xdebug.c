/*
   +----------------------------------------------------------------------+
   | PHP Version 4                                                        |
   +----------------------------------------------------------------------+
   | Copyright (c) 1997, 1998, 1999, 2000, 2001 The PHP Group             |
   +----------------------------------------------------------------------+
   | This source file is subject to version 2.02 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available at through the world-wide-web at                           |
   | http://www.php.net/license/2_02.txt.                                 |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Authors:  Derick Rethans <d.rethans@jdimedia.nl>                     |
   +----------------------------------------------------------------------+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

#if HAVE_XDEBUG

#ifndef PHP_WIN32
#include <sys/time.h>
#else
#include "win32/time.h"
#endif

#include "TSRM.h"
#include "SAPI.h"
#include "php_ini.h"
#include "ext/standard/html.h"
#include "ext/standard/info.h"
#include "ext/standard/php_smart_str.h"
#include "php_globals.h"

#include "zend.h"
#include "zend_API.h"
#include "zend_execute.h"
#include "zend_compile.h"
#include "zend_extensions.h"

#include "xdebug_com.h"
#include "xdebug_llist.h"
#include "xdebug_var.h"
#include "usefulstuff.h"
#include "php_xdebug.h"

/* static int le_xdebug; */

static void xdebug_start_trace();

zend_op_array* (*old_compile_file)(zend_file_handle* file_handle, int type TSRMLS_DC);
zend_op_array* xdebug_compile_file(zend_file_handle*, int TSRMLS_DC);

void (*old_execute)(zend_op_array *op_array TSRMLS_DC);
void xdebug_execute(zend_op_array *op_array TSRMLS_DC);

void (*old_execute_internal)(zend_execute_data *current_execute_data, int return_value_used TSRMLS_DC);
void xdebug_execute_internal(zend_execute_data *current_execute_data, int return_value_used TSRMLS_DC);

void (*old_error_cb)(int type, const char *error_filename, const uint error_lineno, const char *format, va_list args);
void (*new_error_cb)(int type, const char *error_filename, const uint error_lineno, const char *format, va_list args);
void xdebug_error_cb(int type, const char *error_filename, const uint error_lineno, const char *format, va_list args);

static inline zval *get_zval(znode *node, temp_variable *Ts, int *is_var);

function_entry xdebug_functions[] = {
	PHP_FE(xdebug_get_function_stack,  NULL)
	PHP_FE(xdebug_call_function,       NULL)
	PHP_FE(xdebug_call_file,           NULL)
	PHP_FE(xdebug_call_line,           NULL)

	PHP_FE(xdebug_enable,              NULL)
	PHP_FE(xdebug_disable,             NULL)
	PHP_FE(xdebug_is_enabled,          NULL)

	PHP_FE(xdebug_start_trace,         NULL)
	PHP_FE(xdebug_stop_trace,          NULL)
	PHP_FE(xdebug_get_function_trace,  NULL)
	PHP_FE(xdebug_dump_function_trace, NULL)
#if MEMORY_LIMIT
	PHP_FE(xdebug_memory_usage,        NULL)
#endif
	{NULL, NULL, NULL}
};

zend_module_entry xdebug_module_entry = {
	STANDARD_MODULE_HEADER,
	"xdebug",
	xdebug_functions,
	PHP_MINIT(xdebug),
	PHP_MSHUTDOWN(xdebug),
	PHP_RINIT(xdebug),
	PHP_RSHUTDOWN(xdebug),
	PHP_MINFO(xdebug),
	"0.1",
	NULL,
	NULL,
	STANDARD_MODULE_PROPERTIES_EX
};


ZEND_DECLARE_MODULE_GLOBALS(xdebug)

#if COMPILE_DL_XDEBUG
ZEND_GET_MODULE(xdebug)
#endif

static PHP_INI_MH(OnUpdateDebugMode)
{
	if (!new_value) {
		XG(remote_mode) = XDEBUG_NONE;

	} else if (strcmp(new_value, "jit") == 0) {
		XG(remote_mode) = XDEBUG_JIT;

	} else if (strcmp(new_value, "req") == 0) {
		XG(remote_mode) = XDEBUG_REQ;

	} else {
		XG(remote_mode) = XDEBUG_NONE;
	}
	return SUCCESS;
}
	
PHP_INI_BEGIN()
	STD_PHP_INI_ENTRY("xdebug.max_nesting_level", "64",                 PHP_INI_SYSTEM, OnUpdateInt,    max_nesting_level, zend_xdebug_globals, xdebug_globals)
	STD_PHP_INI_BOOLEAN("xdebug.default_enable",  "1",                  PHP_INI_SYSTEM, OnUpdateBool,   default_enable,    zend_xdebug_globals, xdebug_globals)
	STD_PHP_INI_BOOLEAN("xdebug.collect_params",  "1",                  PHP_INI_SYSTEM, OnUpdateBool,   collect_params,    zend_xdebug_globals, xdebug_globals)
	STD_PHP_INI_BOOLEAN("xdebug.auto_trace",      "0",                  PHP_INI_SYSTEM, OnUpdateBool,   auto_trace,        zend_xdebug_globals, xdebug_globals)
	STD_PHP_INI_ENTRY("xdebug.manual_url",        "http://www.php.net", PHP_INI_SYSTEM, OnUpdateString, manual_url,        zend_xdebug_globals, xdebug_globals)

	/* Remote debugger settings */
	STD_PHP_INI_BOOLEAN("xdebug.remote_enable",   "1",                  PHP_INI_SYSTEM, OnUpdateBool,   remote_enable,     zend_xdebug_globals, xdebug_globals)
	STD_PHP_INI_ENTRY("xdebug.remote_port",       "17869",              PHP_INI_ALL,    OnUpdateInt,    remote_port,       zend_xdebug_globals, xdebug_globals)
	STD_PHP_INI_ENTRY("xdebug.remote_host",       "localhost",          PHP_INI_ALL,    OnUpdateString, remote_host,       zend_xdebug_globals, xdebug_globals)
	STD_PHP_INI_ENTRY("xdebug.remote_handler",    "gdb",                PHP_INI_ALL,    OnUpdateString, remote_handler,    zend_xdebug_globals, xdebug_globals)
	PHP_INI_ENTRY("xdebug.remote_mode",           "req",                PHP_INI_ALL,    OnUpdateDebugMode)
PHP_INI_END()

#define MICRO_IN_SEC 1000000.00
	
static double get_utime()
{
#ifdef HAVE_GETTIMEOFDAY
	struct timeval tp;
	long sec = 0L;
	double msec = 0.0;

	if (gettimeofday((struct timeval *) &tp, NULL) == 0) {
		sec = tp.tv_sec;
		msec = (double) (tp.tv_usec / MICRO_IN_SEC);

		if (msec >= 1.0) msec -= (long) msec;
		return msec + sec;
	} else {
#endif
		return 0;
	}
}

static void php_xdebug_init_globals (zend_xdebug_globals *xg)
{
	xg->stack    = NULL;
	xg->level    = 0;
	xg->do_trace = 0;
}

PHP_MINIT_FUNCTION(xdebug)
{
	ZEND_INIT_MODULE_GLOBALS(xdebug, php_xdebug_init_globals, NULL);
	REGISTER_INI_ENTRIES();

	old_compile_file = zend_compile_file;
	zend_compile_file = xdebug_compile_file;

	old_execute = zend_execute;
	zend_execute = xdebug_execute;

	old_execute_internal = zend_execute_internal;
	zend_execute_internal = xdebug_execute_internal;

	old_error_cb = zend_error_cb;
	new_error_cb = xdebug_error_cb;

	return SUCCESS;
}


PHP_MSHUTDOWN_FUNCTION(xdebug)
{
	zend_compile_file = old_compile_file;
	zend_execute = old_execute;
	zend_execute_internal = old_execute_internal;
	zend_error_cb = old_error_cb;

	return SUCCESS;
}

void used_var_dtor (void *elem)
{
	char *s = elem;

	if (s) {
		xdfree(s);
	}
}

void stack_element_dtor (void *dummy, void *elem)
{
	int                   i;
	function_stack_entry *e = elem;

	e->refcount--;

	if (e->refcount == 0) {
		if (e->function.function) {
			xdfree (e->function.function);
		}
		if (e->function.class) {
			xdfree (e->function.class);
		}
		if (e->filename) {
			xdfree (e->filename);
		}

		for (i = 0; i < e->varc; i++) {
			if ((e->vars[i]).name) {
				xdfree ((e->vars[i]).name);
			}
			xdfree ((e->vars[i]).value);
		}

		if (e->used_vars) {
			xdebug_hash_destroy(e->used_vars);
		}

		xdfree (e);
	}
}



PHP_RINIT_FUNCTION(xdebug)
{
	CG(extended_info) = 1;
	XG(level)    = 0;
	XG(do_trace) = 0;
	XG(stack)    = xdebug_llist_alloc (stack_element_dtor);
	XG(trace_file) = NULL;

	if (XG(default_enable)) {
		zend_error_cb = new_error_cb;
	}
	XG(remote_enabled) = 0;
	if (XG(auto_trace)) {
		xdebug_start_trace();
	}

	/* Initialize some debugger context properties */
	XG(context).list.last_file = NULL;
	XG(context).list.last_line = 0;
	XG(context).do_break       = 0;
	XG(context).do_step        = 0;

	return SUCCESS;
}



PHP_RSHUTDOWN_FUNCTION(xdebug)
{
	xdebug_llist_destroy (XG(stack), NULL);
	XG(stack) = NULL;

	if (XG(do_trace)) {
		xdebug_llist_destroy (XG(trace), NULL);
		XG(trace) = NULL;
	}

	if (XG(trace_file)) {
		fprintf (XG(trace_file), "End of function trace\n");
		fclose (XG(trace_file));
	}

	XG(level)    = 0;
	XG(do_trace) = 0;

	if (XG(remote_enabled)) {
		XG(context).handler->remote_deinit(&(XG(context)));
		xdebug_close_socket(XG(context).socket); 
		if (XG(context).program_name) {
			xdfree(XG(context).program_name);
		}
	}

	if (XG(context.list.last_file)) {
		xdfree(XG(context).list.last_file);
	}

	return SUCCESS;
}


PHP_MINFO_FUNCTION(xdebug)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "xdebug support", "enabled");
	php_info_print_table_row(2, "Version", XDEBUG_VERSION);
	php_info_print_table_row(2, "Stacktraces support", "enabled");
	php_info_print_table_row(2, "Function nesting protection support", "enabled");
	php_info_print_table_end();

	DISPLAY_INI_ENTRIES();
}

xdebug_func xdebug_build_fname(zend_execute_data *edata, zend_op_array *new_op_array TSRMLS_DC)
{
	xdebug_func tmp;

	memset(&tmp, 0, sizeof(xdebug_func));

	if (edata) {
		if (edata->function_state.function->common.function_name) {
			if (edata->ce) {
				tmp.type = XFUNC_STATIC_MEMBER;
				tmp.class = xdstrdup(edata->ce->name);
				tmp.function = xdstrdup(edata->function_state.function->common.function_name);
			} else if (edata->object.ptr) {
				tmp.type = XFUNC_MEMBER;
				tmp.class = xdstrdup(edata->object.ptr->value.obj.ce->name);
				tmp.function = xdstrdup(edata->function_state.function->common.function_name);
			} else {
				tmp.type = XFUNC_NORMAL;
				tmp.function = xdstrdup(edata->function_state.function->common.function_name);
			}
		} else {
#if 0
			char *tmpname = "?!?";
			int is_include = 1;
			switch (edata->opline->op2.u.constant.value.lval) {
				case ZEND_EVAL:
					is_include = 0;
					tmpname = "eval";
					break;
				case ZEND_INCLUDE:
					tmpname = "include";
					break;
				case ZEND_REQUIRE:
					tmpname = "require";
					break;
				case ZEND_INCLUDE_ONCE:
					tmpname = "include_once";
					break;
				case ZEND_REQUIRE_ONCE:
					tmpname = "require_once";
					break;
				default:
					assert(0);
					break;
			}

			if (is_include && new_op_array) {
				sprintf(fname, "%s %s", tmpname, new_op_array->filename);
			} else {
				sprintf(fname, "%s", tmpname);
			}
#endif
			switch (edata->opline->op2.u.constant.value.lval) {
				case ZEND_EVAL:
					tmp.type = XFUNC_EVAL;
					break;
				case ZEND_INCLUDE:
					tmp.type = XFUNC_INCLUDE;
					break;
				case ZEND_REQUIRE:
					tmp.type = XFUNC_REQUIRE;
					break;
				case ZEND_INCLUDE_ONCE:
					tmp.type = XFUNC_INCLUDE_ONCE;
					break;
				case ZEND_REQUIRE_ONCE:
					tmp.type = XFUNC_REQUIRE_ONCE;
					break;
				default:
					assert(0);
					break;
			}
		}
	}
	return tmp; 
}


static struct function_stack_entry *add_stack_frame(zend_execute_data *zdata, zend_op_array *op_array, int type TSRMLS_DC)
{
	struct function_stack_entry  *tmp;
	zend_op *cur_opcode;
	zval                **param;
	void                **p         = EG(argument_stack).top_element-2;
	int                   arg_count = (ulong) *p;
	int                   i         = 0;

	tmp = xdmalloc (sizeof (struct function_stack_entry));
	tmp->varc     = 0;
	tmp->refcount = 1;
	tmp->level    = XG(level);
	tmp->arg_done      = 0;
	tmp->used_vars     = NULL;
	tmp->user_defined  = type;

	if (EG(current_execute_data) && EG(current_execute_data)->op_array) {
		tmp->filename  = xdstrdup(EG(current_execute_data)->op_array->filename);
	} else {
		tmp->filename  = (op_array && op_array->filename) ? xdstrdup(op_array->filename): NULL;
	}
#if MEMORY_LIMIT
	tmp->memory    = AG(allocated_memory);
#else
	tmp->memory    = 0;
#endif
	tmp->time      = get_utime();

	tmp->function = xdebug_build_fname(zdata, op_array TSRMLS_CC);
	if (!tmp->function.type) {
		tmp->function.function = xdstrdup("{main}");
		tmp->function.class    = NULL;
		tmp->function.type     = XFUNC_NORMAL;
		tmp->lineno = 0;

	} else if (tmp->function.type == XFUNC_INCLUDE ||
		tmp->function.type == XFUNC_REQUIRE ||
		tmp->function.type == XFUNC_INCLUDE_ONCE ||
		tmp->function.type == XFUNC_REQUIRE_ONCE ||
		tmp->function.type == XFUNC_EVAL
	) {
		zval *param;
		int   is_var;

		cur_opcode = *EG(opline_ptr);
		tmp->lineno = cur_opcode->lineno;

		param = get_zval(&zdata->opline->op1, zdata->Ts, &is_var);
		tmp->vars[tmp->varc].name  = NULL;
		tmp->vars[tmp->varc].value = xdstrdup(param->value.str.val);
		tmp->varc++;

	} else {
		cur_opcode = *EG(opline_ptr);
		tmp->lineno = cur_opcode->lineno;
		if (XG(collect_params)) {
			for (i = 0; i < arg_count; i++) {
				tmp->vars[tmp->varc].name  = NULL;
				if (zend_ptr_stack_get_arg(tmp->varc + 1, (void**) &param TSRMLS_CC) == SUCCESS) {
					tmp->vars[tmp->varc].value = get_zval_value(*param);
				} else {
					tmp->vars[tmp->varc].value = xdstrdup ("{missing}");
				}
				tmp->varc++;
			}
		}
	}
	xdebug_llist_insert_next (XG(stack), XDEBUG_LLIST_TAIL(XG(stack)), tmp);

	if (XG(do_trace)) {
		tmp->refcount++;
		xdebug_llist_insert_next (XG(trace), XDEBUG_LLIST_TAIL(XG(trace)), tmp);
	}
	return tmp;
}

static void add_used_variables (struct function_stack_entry *fse, zend_op_array *op_array)
{
	int i = 0; 
	int j = op_array->size;

	fse->used_vars = xdebug_hash_alloc(64, used_var_dtor); 
	while (i < j) {
		if (op_array->opcodes[i].opcode == ZEND_FETCH_R || op_array->opcodes[i].opcode == ZEND_FETCH_W) {
			if (op_array->opcodes[i].op1.op_type == IS_CONSTANT) {
				xdebug_hash_update(
					fse->used_vars, 
					op_array->opcodes[i].op1.u.constant.value.str.val,
					op_array->opcodes[i].op1.u.constant.value.str.len,
					xdstrdup(op_array->opcodes[i].op1.u.constant.value.str.val)
				);
			}
		}
		i++;
	}
}

static int handle_breakpoints (struct function_stack_entry *fse)
{
	char *name     = NULL;
	char *tmp_name = NULL;
	TSRMLS_FETCH();

	/* Function breakpoints */
	if (fse->function.type == XFUNC_NORMAL) {
		if (xdebug_hash_find(XG(context).function_breakpoints, fse->function.function, strlen(fse->function.function), (void *) &name)) {
			/* Yup, breakpoint found, call handler */
			if (fse->user_defined == XDEBUG_EXTERNAL) {
				XG(context).do_break = 1;
			} else {
				if (!XG(context).handler->remote_breakpoint(&(XG(context)), XG(stack), fse->filename, fse->lineno, XDEBUG_BREAK)) {
					XG(remote_enabled) = 0;
					XG(remote_enable)  = 0;
					return 0;
				}
			}
		}
	}
	/* class->function breakpoints */
	else if (fse->function.type == XFUNC_MEMBER || fse->function.type == XFUNC_STATIC_MEMBER) {
		if (fse->function.type == XFUNC_MEMBER) {
			tmp_name = xdebug_sprintf ("%s->%s", fse->function.class, fse->function.function);
		} else if (fse->function.type == XFUNC_STATIC_MEMBER) {
			tmp_name = xdebug_sprintf ("%s::%s", fse->function.class, fse->function.function);
		}

		if (xdebug_hash_find(XG(context).class_breakpoints, tmp_name, strlen(tmp_name), (void *) &name)) {
			/* Yup, breakpoint found, call handler */
			XG(context).do_break = 1;
		}
		xdfree(tmp_name);
	}
	return 1;
}

void xdebug_execute(zend_op_array *op_array TSRMLS_DC)
{
/*	zval                        **dummy; */
	zend_execute_data           *edata = EG(current_execute_data);
	struct function_stack_entry *fse;
		
	/* Start context if requested */
	if (
		!XG(remote_enabled) &&
		XG(remote_enable) &&
		(XG(remote_mode) == XDEBUG_REQ) /* &&
		PG(http_globals)[TRACK_VARS_GET] &&
		zend_hash_find(PG(http_globals)[TRACK_VARS_GET]->value.ht, "__XDEBUG__", sizeof("__XDEBUG__"), (void **) &dummy) == SUCCESS */
	) {
		XG(context).socket = xdebug_create_socket(XG(remote_host), XG(remote_port));
		if (XG(context).socket >= 0) {
			XG(remote_enabled) = 1;

			/* Get handler from mode */
			XG(context).handler = xdebug_handler_get(XG(remote_handler));
			XG(context).program_name = xdstrdup(op_array->filename);
			if (!XG(context).handler->remote_init(&(XG(context)), XDEBUG_REQ)) {
				XG(remote_enabled) = 0;
				XG(remote_enable)  = 0;
			}
		}
	}

	XG(level)++;
	fse = add_stack_frame(edata, op_array, XDEBUG_EXTERNAL TSRMLS_CC);

	if (XDEBUG_IS_FUNCTION(fse->function.type)) {
		add_used_variables(fse, op_array);
	}

	/* Check for breakpoints */
	if (XG(remote_enabled)) {
		if (!handle_breakpoints(fse)) {
			XG(remote_enabled) = 0;
			XG(remote_enable)  = 0;
		}
	}

	old_execute (op_array TSRMLS_CC);
	xdebug_llist_remove (XG(stack), XDEBUG_LLIST_TAIL(XG(stack)), stack_element_dtor);
	XG(level)--;
}

void xdebug_execute_internal(zend_execute_data *current_execute_data, int return_value_used TSRMLS_DC)
{
/*	zval                        **dummy; */
	zend_execute_data           *edata = EG(current_execute_data);
	struct function_stack_entry *fse;

	XG(level)++;
	fse = add_stack_frame(edata, edata->op_array, XDEBUG_INTERNAL TSRMLS_CC);

	/* Check for breakpoints */
	if (XG(remote_enabled)) {
		if (!handle_breakpoints(fse)) {
			XG(remote_enabled) = 0;
			XG(remote_enable)  = 0;
		}
	}

	execute_internal(current_execute_data, return_value_used TSRMLS_CC);
	xdebug_llist_remove (XG(stack), XDEBUG_LLIST_TAIL(XG(stack)), stack_element_dtor);
	XG(level)--;
}

static inline void print_stack (int html, const char *error_type_str, char *buffer, const char *error_filename, const int error_lineno TSRMLS_DC)
{
	char *error_format;
	xdebug_llist_element *le;
	int new_len;
	int is_cli = (strcmp ("cli", sapi_module.name) == 0);
	double start_time = 0;

	if (html) {
		php_printf ("<br />\n<table border='1' cellspacing='0'>\n");
	}

	error_format = html ?
		"<tr><td bgcolor='#ffbbbb' colspan=\"3\"><b>%s</b>: %s in <b>%s</b> on line <b>%d</b><br />\n"
		: "\n%s: %s in %s on line %d\n";
	php_printf(error_format, error_type_str, buffer, error_filename, error_lineno);

	if (XG(stack)) {
		if (html) {
			php_printf ("<tr><th bgcolor='#aaaaaa' colspan='3'>Call Stack</th></tr>\n");
			php_printf ("<tr><th bgcolor='#cccccc'>#</th><th bgcolor='#cccccc'>Function</th><th bgcolor='#cccccc'>Location</th></tr>\n");
		} else {
			printf ("\nCall Stack:\n");
		}

		if (PG(log_errors) && !is_cli) {
			php_log_err("PHP Stack trace:" TSRMLS_CC);
		}

		for (le = XDEBUG_LLIST_HEAD(XG(stack)); le != NULL; le = XDEBUG_LLIST_NEXT(le))
		{
			int c = 0; /* Comma flag */
			int j = 0; /* Counter */
			struct function_stack_entry *i = XDEBUG_LLIST_VALP(le);
			char *tmp_name;
			char log_buffer[4096];
			
			tmp_name = show_fname (i TSRMLS_CC);
			if (html) {
				php_printf ("<tr><td bgcolor='#ffffff' align='center'>%d</td><td bgcolor='#ffffff'>%s(", i->level, tmp_name);
			} else {
				if (start_time) {
					php_printf ("%10.4f ", i->time - start_time);
				} else {
					start_time = i->time;
					php_printf ("%10.4f ", 0.0);
				}
				php_printf ("%10lu ", i->memory);
				printf ("%3d. %s(", i->level, tmp_name);
			}
			if (PG(log_errors) && !is_cli) {
				snprintf(log_buffer, 1024, "PHP %3d. %s(", i->level, tmp_name);
			}
			xdfree (tmp_name);

			/* Printing vars */
			for (j = 0; j < i->varc; j++) {
				char *tmp_varname;

				if (c) {
					if (html) {
						php_printf (", ");
					} else {
						printf (", ");
					}
					if (PG(log_errors) && !is_cli) {
						strcat(log_buffer, ", ");
					}
				} else {
					c = 1;
				}
				tmp_varname = i->vars[j].name ? xdebug_sprintf ("$%s = ", i->vars[j].name) : xdstrdup("");
				if (html) {
					php_printf ("%s%s", tmp_varname,
						php_escape_html_entities (i->vars[j].value, strlen(i->vars[j].value), &new_len, 1, 1, NULL TSRMLS_CC));
				} else {
					printf ("%s%s", tmp_varname, i->vars[j].value);
				}
				if (PG(log_errors) && !is_cli) {
					snprintf(
						log_buffer + strlen(log_buffer),
						1024 - strlen(log_buffer),
						"%s%s", tmp_varname, i->vars[j].value
					);
				}
				xdfree(tmp_varname);
			}

			if (html) {
				php_printf (")</td><td bgcolor='#ffffff'>%s<b>:</b>%d</td></tr>\n", i->filename, i->lineno);
			} else {
				printf (") %s:%d\n", i->filename, i->lineno);
			}
			if (PG(log_errors) && !is_cli) {
				snprintf(
					log_buffer + strlen(log_buffer),
					1024 - strlen(log_buffer),
					") %s:%d", i->filename, i->lineno
				);
				php_log_err(log_buffer TSRMLS_CC);
			}
		}

		if (html) {
			php_printf ("</table>\n");
		}
	}
}

static inline void print_trace (int html TSRMLS_DC)
{
	xdebug_llist_element *le;
	int new_len;
	double start_time = 0;

	if (XG(trace)) {
		if (html) {
			php_printf ("<br />\n<table border='1' cellspacing='0'>\n");
		} else {
			printf ("\nFunction trace:\n");
		}

		if (html) {
#if MEMORY_LIMIT
			php_printf ("<tr><th bgcolor='#aaaaaa' colspan='5'>Function trace</th></tr>\n");
			php_printf ("<tr><th bgcolor='#cccccc'>Time</th><th bgcolor='#cccccc'>#</th><th bgcolor='#cccccc'>Function</th><th bgcolor='#cccccc'>Location</th><th bgcolor='#cccccc'>Memory</th></tr>\n");
#else
			php_printf ("<tr><th bgcolor='#aaaaaa' colspan='4'>Function trace</th></tr>\n");
			php_printf ("<tr><th bgcolor='#cccccc'>Time</th><th bgcolor='#cccccc'>#</th><th bgcolor='#cccccc'>Function</th><th bgcolor='#cccccc'>Location</th></tr>\n");
#endif
		}

		for (le = XDEBUG_LLIST_HEAD(XG(trace)); le != NULL; le = XDEBUG_LLIST_NEXT(le))
		{
			int c = 0; /* Comma flag */
			int j = 0; /* Counter */
			struct function_stack_entry *i = XDEBUG_LLIST_VALP(le);
			char *tmp_name;

			if (i->function.function && strcmp (i->function.function, "xdebug_dump_function_trace") == 0) {
				return;
			}

			tmp_name = show_fname(i TSRMLS_CC);

			if (html) {
				/* Start row */
				php_printf ("<tr>");

				/* Do timestamp */
				php_printf ("<td bgcolor='#ffffff' align='center'>");
				if (start_time) {
					php_printf ("%.6f", i->time - start_time);
				} else {
					start_time = i->time;
					php_printf ("0");
				}
				php_printf ("</td>");

				/* Do rest of line */
				php_printf ("<td bgcolor='#ffffff' align='left'><pre>");
				for (j = 0; j < i->level - 1; j++) {
					php_printf ("  ");
				}
				php_printf ("-></pre></td><td bgcolor='#ffffff'>%s(", tmp_name);
			} else {
				if (start_time) {
					php_printf ("%10.4f ", i->time - start_time);
				} else {
					start_time = i->time;
					php_printf ("%10.4f ", 0.0);
				}
				php_printf ("%10lu ", i->memory);
				for (j = 0; j < i->level; j++) {
					printf ("  ");
				}
				printf ("-> %s(", tmp_name);
			}
			xdfree (tmp_name);

			/* Printing vars */
			for (j = 0; j < i->varc; j++) {
				char *tmp_varname;

				if (c) {
					if (html) {
						php_printf (", ");
					} else {
						printf (", ");
					}
				} else {
					c = 1;
				}

				tmp_varname = i->vars[j].name ? xdebug_sprintf ("$%s = ", i->vars[j].name) : xdstrdup("");
				if (html) {
					php_printf ("%s%s", tmp_varname,
						php_escape_html_entities (i->vars[j].value, strlen(i->vars[j].value), &new_len, 1, 1, NULL TSRMLS_CC));
				} else {
					printf ("%s%s", tmp_varname, i->vars[j].value);
				}
				xdfree (tmp_varname);
			}

			if (html) {
				/* Do filename and line no */
				php_printf (")</td><td bgcolor='#ffffff'>%s<b>:</b>%d</td>", i->filename, i->lineno);
#if MEMORY_LIMIT
				/* Do memory */
				php_printf ("<td bgcolor='#ffffff' align='right'>%lu</td>", i->memory);
#endif
				/* Close row */
				php_printf ("</tr>\n");
			} else {
				printf (") %s:%d\n", i->filename, i->lineno);
			}
		}

		if (html) {
			php_printf ("</table>\n");
		}
	}
}

void xdebug_error_cb(int type, const char *error_filename, const uint error_lineno, const char *format, va_list args)
{
	char *error_type_str;
	int buffer_len;
	char buffer[1024];

	TSRMLS_FETCH();

	buffer_len = vsnprintf(buffer, sizeof(buffer)-1, format, args);
	buffer[sizeof(buffer)-1]=0;
	if (buffer_len > sizeof(buffer) - 1 || buffer_len < 0) {
		buffer_len = sizeof(buffer) - 1;
	}

	error_type_str = error_type(type);

	if (EG(error_reporting) & type) {
		print_stack (!(strcmp ("cli", sapi_module.name) == 0), error_type_str, buffer, error_filename, error_lineno TSRMLS_CC);
	}

	/* Log to logger */
	if (PG(log_errors) && !(strcmp ("cli", sapi_module.name) == 0)) {
		char log_buffer[1024];

#ifdef PHP_WIN32
		if (type==E_CORE_ERROR || type==E_CORE_WARNING) {
			MessageBox(NULL, buffer, error_type_str, MB_OK|ZEND_SERVICE_MB_STYLE);
		}
#endif
		snprintf(log_buffer, 1024, "PHP %s:  %s in %s on line %d", error_type_str, buffer, error_filename, error_lineno);
		php_log_err(log_buffer TSRMLS_CC);
	}
	xdfree(error_type_str);

	/* Start JIT if requested and not yet enabled */
	if (XG(remote_enable) && (XG(remote_mode) == XDEBUG_JIT) && !XG(remote_enabled)) {
		XG(context).socket = xdebug_create_socket(XG(remote_host), XG(remote_port));
		if (XG(context).socket >= 0) {
			XG(remote_enabled) = 1;
			XG(context).program_name = NULL;

			/* Get handler from mode */
			XG(context).handler = xdebug_handler_get(XG(remote_handler));
			XG(context).handler->remote_init(&(XG(context)), XDEBUG_JIT);
		}
	}
	if (XG(remote_enabled)) {
		if (!XG(context).handler->remote_error(&(XG(context)), type, buffer, error_filename, error_lineno, XG(stack))) {
			XG(remote_enabled) = 0;
			XG(remote_enable)  = 0;
		}
	}

	/* Bail out if we can't recover */
	switch (type) {
		case E_CORE_ERROR:
		/* no break - intentionally */
		case E_ERROR:
		/*case E_PARSE: the parser would return 1 (failure), we can bail out nicely */
		case E_COMPILE_ERROR:
		case E_USER_ERROR:
			zend_bailout();
			break;
	}

}

/* {{{ zend_op_array srm_compile_file (file_handle, type)
 *    This function provides a hook for the execution of bananas */
zend_op_array *xdebug_compile_file(zend_file_handle *file_handle, int type TSRMLS_DC)
{
	zend_op_array *op_array;

	op_array = old_compile_file (file_handle, type TSRMLS_CC);

	return op_array;
}
/* }}} */

PHP_FUNCTION(xdebug_get_function_stack)
{
	xdebug_llist_element *le;
	int                   j;
	unsigned int          k;
	zval                 *frame;
	zval                 *params;

	array_init(return_value);
	le = XDEBUG_LLIST_HEAD(XG(stack));

	for (k = 0; k < XG(stack)->size - 1; k++, le = XDEBUG_LLIST_NEXT(le)) {
		struct function_stack_entry *i = XDEBUG_LLIST_VALP(le);

		if (i->function.function) {
			if (strcmp (i->function.function, "xdebug_get_function_stack") == 0) {
				return;
			}
		}

		/* Initialize frame array */
		MAKE_STD_ZVAL(frame);
		array_init(frame);

		/* Add data */
		if (i->function.function) {
			add_assoc_string_ex(frame, "function", sizeof("function"), i->function.function, 1);
		}
		if (i->function.class) {
			add_assoc_string_ex(frame, "class",    sizeof("class"),    i->function.class,    1);
		}
		add_assoc_string_ex(frame, "file", sizeof("file"), i->filename, 1);
		add_assoc_long_ex  (frame, "line", sizeof("line"), i->lineno);

		/* Add parameters */
		MAKE_STD_ZVAL(params);
		array_init(params);
		for (j = 0; j < i->varc; j++) {
			if (i->vars[j].name) {
				add_assoc_string_ex(params, i->vars[j].name, strlen (i->vars[j].name) + 1, i->vars[j].value, 1);
			} else {
				add_index_string(params, j, i->vars[j].value, 1);
			}
		}
		add_assoc_zval_ex  (frame, "params", sizeof("params"), params);

		add_next_index_zval(return_value, frame);
	}
}

PHP_FUNCTION(xdebug_call_function)
{
	xdebug_llist_element        *le;
	struct function_stack_entry *i;

	le = XDEBUG_LLIST_TAIL(XG(stack));
	if (le) {
		if (le->prev) {
			le = XDEBUG_LLIST_PREV(le);
			if (le->prev) {
				le = XDEBUG_LLIST_PREV(le);
			}
		}
		i = XDEBUG_LLIST_VALP(le);

		RETURN_STRING(i->function.function ? i->function.function : "{}", 1);
	} else {
		RETURN_FALSE;
	}
}

PHP_FUNCTION(xdebug_call_line)
{
	xdebug_llist_element        *le;
	struct function_stack_entry *i;

	le = XDEBUG_LLIST_TAIL(XG(stack));
	if (le) {
		if (le->prev) {
			le = XDEBUG_LLIST_PREV(le);
		}
		i = XDEBUG_LLIST_VALP(le);

		RETURN_LONG(i->lineno);
	} else {
		RETURN_FALSE;
	}
}

PHP_FUNCTION(xdebug_call_file)
{
	xdebug_llist_element        *le;
	struct function_stack_entry *i;

	le = XDEBUG_LLIST_TAIL(XG(stack));
	if (le) {
		if (le->prev) {
			le = XDEBUG_LLIST_PREV(le);
		}
		i = XDEBUG_LLIST_VALP(le);

		RETURN_STRING(i->filename, 1);
	} else {
		RETURN_FALSE;
	}
}

PHP_FUNCTION(xdebug_enable)
{
	zend_error_cb = new_error_cb;
}

PHP_FUNCTION(xdebug_disable)
{
	zend_error_cb = old_error_cb;
}

PHP_FUNCTION(xdebug_is_enabled)
{
	RETURN_BOOL(zend_error_cb == new_error_cb);
}

static void xdebug_start_trace()
{
	TSRMLS_FETCH();
	XG(trace)    = xdebug_llist_alloc (stack_element_dtor);
	XG(do_trace) = 1;
}


PHP_FUNCTION(xdebug_start_trace)
{
	char *fname = NULL;
	int   fname_len = 0;

	if (XG(do_trace) == 0) {
		if (zend_parse_parameters (ZEND_NUM_ARGS() TSRMLS_CC, "|s", &fname, &fname_len) == FAILURE) {
			return;
		}

		xdebug_start_trace();

		if (fname) {
			XG(trace_file) = fopen (fname, "a");
			if (XG(trace_file)) {
				fprintf (XG(trace_file), "\nStart of function trace\n");
			}
		} else {
			XG(trace_file) = NULL;
		}
	} else {
		php_error (E_NOTICE, "Function trace already started");
	}
}

PHP_FUNCTION(xdebug_stop_trace)
{
	if (XG(do_trace) == 1) {
		XG(do_trace) = 0;
		xdebug_llist_destroy (XG(trace), NULL);
		XG(trace)    = NULL;
		if (XG(trace_file)) {
			fprintf (XG(trace_file), "End of function trace\n");
			fclose (XG(trace_file));
		}
	} else {
		php_error (E_NOTICE, "Function trace was not started");
	}
}

PHP_FUNCTION(xdebug_dump_function_trace)
{
	if (XG(do_trace)) {
		print_trace (PG(html_errors) TSRMLS_CC);
	} else {
		php_error (E_NOTICE, "Function tracing was not started, use xdebug_start_trace() before calling this function");
	}
}

PHP_FUNCTION(xdebug_get_function_trace)
{
	xdebug_llist_element *le;
	int                   j;
	unsigned int          k;
	zval                 *frame;
	zval                 *params;

	if (!XG(do_trace)) {
		php_error (E_NOTICE, "Function tracing was not started, use xdebug_start_trace() before calling this function");
		RETURN_FALSE;
	}

	array_init(return_value);
	le = XDEBUG_LLIST_HEAD(XG(trace));
	
	for (k = 0; k < XG(trace)->size - 1; k++, le = XDEBUG_LLIST_NEXT(le)) {
		struct function_stack_entry *i = XDEBUG_LLIST_VALP(le);

		if (i->function.function) {
			if (strcmp (i->function.function, "xdebug_get_function_trace") == 0) {
				return;
			}
		}
		/* Initialize frame array */
		MAKE_STD_ZVAL(frame);
		array_init(frame);

		/* Add data */
		if (i->function.function) {
			add_assoc_string_ex(frame, "function", sizeof("function"), i->function.function, 1);
		} else {
			switch (i->function.type) {
				case XFUNC_NEW:
					add_assoc_string_ex(frame, "function", sizeof("function"), "{new}", 1);
					break;

				case XFUNC_EVAL:
					add_assoc_string_ex(frame, "function", sizeof("function"), "{eval}", 1);
					break;

				case XFUNC_INCLUDE:
					add_assoc_string_ex(frame, "function", sizeof("function"), "{include}", 1);
					break;

				case XFUNC_INCLUDE_ONCE:
					add_assoc_string_ex(frame, "function", sizeof("function"), "{include_once}", 1);
					break;

				case XFUNC_REQUIRE:
					add_assoc_string_ex(frame, "function", sizeof("function"), "{require}", 1);
					break;

				case XFUNC_REQUIRE_ONCE:
					add_assoc_string_ex(frame, "function", sizeof("function"), "{require_once}", 1);
					break;

			}
		}
		if (i->function.class) {
			add_assoc_string_ex(frame, "class", sizeof("class"), i->function.class, 1);
		}
		if (i->filename) {
			add_assoc_string_ex(frame, "file", sizeof("file"), i->filename, 1);
		}
		add_assoc_long_ex  (frame, "line", sizeof("line"), i->lineno);

		/* Add parameters */
		MAKE_STD_ZVAL(params);
		array_init(params);
		for (j = 0; j < i->varc; j++) {
			if (i->vars[j].name) {
				add_assoc_string_ex(params, i->vars[j].name, strlen (i->vars[j].name) + 1, i->vars[j].value, 1);
			} else {
				add_assoc_string_ex(params, "1", sizeof("1"), i->vars[j].value, 1);
			}
		}
		add_assoc_zval_ex  (frame, "params", sizeof("params"), params);

		add_next_index_zval(return_value, frame);
	}
}


#if MEMORY_LIMIT
PHP_FUNCTION(xdebug_memory_usage)
{
	RETURN_LONG(AG(allocated_memory));
}
#endif

/*************************************************************************************************************************************/

static inline zval *get_zval(znode *node, temp_variable *Ts, int *is_var)
{
	switch (node->op_type) {
		case IS_CONST:
			return &node->u.constant;
			break;

		case IS_TMP_VAR:
			*is_var = 1;
			return &Ts[node->u.var].tmp_var;
			break;

		case IS_VAR:
			*is_var = 1;
			if (Ts[node->u.var].var.ptr) {
				return Ts[node->u.var].var.ptr;
			} else {
				fprintf(stderr, "\nIS_VAR\n");
			}
			break;

		case IS_UNUSED:
			fprintf(stderr, "\nIS_UNUSED\n");
			break;

		default:
			fprintf(stderr, "\ndefault %d\n", node->op_type);
			break;
	}

	*is_var = 1;

	return NULL;
}


ZEND_DLEXPORT void xdebug_statement_call (zend_op_array *op_array)
{
	xdebug_llist_element *le;
	xdebug_brk_info      *brk;
	zend_op              *cur_opcode;
	int                   lineno;
	char                 *file;
	int                   file_len = 0;
	TSRMLS_FETCH();

	if (XG(remote_enabled)) {
		TSRMLS_FETCH();
		cur_opcode = *EG(opline_ptr);
		lineno = cur_opcode->lineno;

		file = op_array->filename;
		file_len = strlen(file);

		if (XG(context).do_break) {
			XG(context).do_break = 0;

			if (!XG(context).handler->remote_breakpoint(&(XG(context)), XG(stack), file, lineno, XDEBUG_BREAK)) {
				XG(remote_enabled) = 0;
				XG(remote_enable)  = 0;
				return;
			}
		}

		if (XG(context).do_step) {
			XG(context).do_step = 0;

			if (!XG(context).handler->remote_breakpoint(&(XG(context)), XG(stack), file, lineno, XDEBUG_STEP)) {
				XG(remote_enabled) = 0;
				XG(remote_enable)  = 0;
				return;
			}
		}

		if (XG(context).line_breakpoints) {
			for (le = XDEBUG_LLIST_HEAD(XG(context).line_breakpoints); le != NULL; le = XDEBUG_LLIST_NEXT(le)) {
				brk = XDEBUG_LLIST_VALP(le);

				if (lineno == brk->lineno && memcmp(brk->file, file + file_len - brk->file_len, brk->file_len) == 0) {
					if (!XG(context).handler->remote_breakpoint(&(XG(context)), XG(stack), file, lineno, XDEBUG_BREAK)) {
						XG(remote_enabled) = 0;
						XG(remote_enable)  = 0;
						return;
					}
				}
			}
		}
	}
}


ZEND_DLEXPORT int xdebug_zend_startup(zend_extension *extension)
{
	TSRMLS_FETCH();
	CG(extended_info) = 1;
	return zend_startup_module(&xdebug_module_entry);
}

ZEND_DLEXPORT void xdebug_zend_shutdown(zend_extension *extension)
{
	/* Do nothing. */
}

#ifndef ZEND_EXT_API
#define ZEND_EXT_API    ZEND_DLEXPORT
#endif
ZEND_EXTENSION();

ZEND_DLEXPORT zend_extension zend_extension_entry = {
	"eXtended Debugger (xdebug)",
	XDEBUG_VERSION,
	"Derick Rethans",
	"http://xdebug.derickrethans.nl/",
	"Copyright (c) 2002 JDI Media Solutions",
	xdebug_zend_startup,
	xdebug_zend_shutdown,
	NULL,           /* activate_func_t */
	NULL,           /* deactivate_func_t */
	NULL,           /* message_handler_func_t */
	NULL,           /* op_array_handler_func_t */
	xdebug_statement_call, /* statement_handler_func_t */
	NULL,           /* fcall_begin_handler_func_t */
	NULL,           /* fcall_end_handler_func_t */
	NULL,           /* op_array_ctor_func_t */
	NULL,           /* op_array_dtor_func_t */
	STANDARD_ZEND_EXTENSION_PROPERTIES
};

#endif
