/**************************************************************************/
/*  error_macros.cpp                                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "error_macros.h"

#include "core/io/logger.h"
#include "core/os/os.h"
#include "core/string/ustring.h"

#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>

static ErrorHandlerList *error_handler_list = nullptr;

void add_error_handler(ErrorHandlerList *p_handler) {
	// If p_handler is already in error_handler_list
	// we'd better remove it first then we can add it.
	// This prevent cyclic redundancy.
	remove_error_handler(p_handler);

	_global_lock();

	p_handler->next = error_handler_list;
	error_handler_list = p_handler;

	_global_unlock();
}

void remove_error_handler(const ErrorHandlerList *p_handler) {
	_global_lock();

	ErrorHandlerList *prev = nullptr;
	ErrorHandlerList *l = error_handler_list;

	while (l) {
		if (l == p_handler) {
			if (prev) {
				prev->next = l->next;
			} else {
				error_handler_list = l->next;
			}
			break;
		}
		prev = l;
		l = l->next;
	}

	_global_unlock();
}

// Errors without messages.
void _err_print_error(const char *p_function, const char *p_file, int p_line, const char *p_error, bool p_editor_notify, ErrorHandlerType p_type) {
	_err_print_error(p_function, p_file, p_line, p_error, "", p_editor_notify, p_type);
}

void _err_print_error(const char *p_function, const char *p_file, int p_line, const String &p_error, bool p_editor_notify, ErrorHandlerType p_type) {
	_err_print_error(p_function, p_file, p_line, p_error.utf8().get_data(), "", p_editor_notify, p_type);
}

// Main error printing function.
void _err_print_error(const char *p_function, const char *p_file, int p_line, const char *p_error, const char *p_message, bool p_editor_notify, ErrorHandlerType p_type) {
	if (OS::get_singleton()) {
		OS::get_singleton()->print_error(p_function, p_file, p_line, p_error, p_message, p_editor_notify, (Logger::ErrorType)p_type);
	} else {
		// Fallback if errors happen before OS init or after it's destroyed.
		const char *err_details = (p_message && *p_message) ? p_message : p_error;
		fprintf(stderr, "ERROR: %s\n   at: %s (%s:%i)\n", err_details, p_function, p_file, p_line);
	}

	_global_lock();
	ErrorHandlerList *l = error_handler_list;
	while (l) {
		l->errfunc(l->userdata, p_function, p_file, p_line, p_error, p_message, p_editor_notify, p_type);
		l = l->next;
	}

	_global_unlock();
}

// Errors with message. (All combinations of p_error and p_message as String or char*.)
void _err_print_error(const char *p_function, const char *p_file, int p_line, const String &p_error, const char *p_message, bool p_editor_notify, ErrorHandlerType p_type) {
	_err_print_error(p_function, p_file, p_line, p_error.utf8().get_data(), p_message, p_editor_notify, p_type);
}

void _err_print_error(const char *p_function, const char *p_file, int p_line, const char *p_error, const String &p_message, bool p_editor_notify, ErrorHandlerType p_type) {
	_err_print_error(p_function, p_file, p_line, p_error, p_message.utf8().get_data(), p_editor_notify, p_type);
}

void _err_print_error(const char *p_function, const char *p_file, int p_line, const String &p_error, const String &p_message, bool p_editor_notify, ErrorHandlerType p_type) {
	_err_print_error(p_function, p_file, p_line, p_error.utf8().get_data(), p_message.utf8().get_data(), p_editor_notify, p_type);
}

// Index errors. (All combinations of p_message as String or char*.)
void _err_print_index_error(const char *p_function, const char *p_file, int p_line, int64_t p_index, int64_t p_size, const char *p_index_str, const char *p_size_str, const char *p_message, bool p_editor_notify, bool p_fatal) {
	String fstr(p_fatal ? "FATAL: " : "");
	String err(fstr + "Index " + p_index_str + " = " + itos(p_index) + " is out of bounds (" + p_size_str + " = " + itos(p_size) + ").");
	_err_print_error(p_function, p_file, p_line, err.utf8().get_data(), p_message, p_editor_notify, ERR_HANDLER_ERROR);
}

void _err_print_index_error(const char *p_function, const char *p_file, int p_line, int64_t p_index, int64_t p_size, const char *p_index_str, const char *p_size_str, const String &p_message, bool p_editor_notify, bool p_fatal) {
	_err_print_index_error(p_function, p_file, p_line, p_index, p_size, p_index_str, p_size_str, p_message.utf8().get_data(), p_editor_notify, p_fatal);
}

struct FunctionInfo {
	const char *function;
	const char *file;
	int line;
	String descriptor;
};

FunctionInfo describe_function(const Dl_info &info, const void *address) {
	FunctionInfo result;
	constexpr int kDemangledBufferSize = 100;
	static char s_demangled[kDemangledBufferSize];
	int status;
	char *demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);

	if (status == 0) {
		// Have to do it this way to avoid a memory leak, as abi::__cxa_demangle returns a `malloc`ed c-string.
		strncpy(s_demangled, demangled, kDemangledBufferSize);
		s_demangled[kDemangledBufferSize - 1] = 0;
		result.function = s_demangled;
	} else {
		result.function = info.dli_sname;
	}
	free(demangled);

	result.file = info.dli_fname;
	result.line = static_cast<const char *>(info.dli_saddr) - static_cast<const char *>(info.dli_fbase);

#ifdef DEBUG_ENABLED
	if (OS::get_singleton()->get_name() == "macOS") {
		String pipe;
		Error error = OS::get_singleton()->execute("atos", { "-o", info.dli_fname, "-l", String::num_uint64(reinterpret_cast<uint64_t>(info.dli_fbase), 16), String::num_uint64(reinterpret_cast<uint64_t>(address), 16) },
				&pipe);

		if (error == OK) {
			result.descriptor = pipe + " - ";
		}
	}
#endif

	return result;
}

FunctionInfo calling_function(const char *filter, int frames_to_skip = 0) {
	constexpr int kBacktraceDepth = 15;
	void *backtrace_addrs[kBacktraceDepth];
	Dl_info info;

	int trace_size = backtrace(backtrace_addrs, kBacktraceDepth);

	int i = frames_to_skip;
	do {
		++i;
		dladdr(backtrace_addrs[i], &info);
	} while (i < trace_size && strstr(info.dli_sname, filter));

	return describe_function(info, backtrace_addrs[i]);
}

void _err_print_callstack(const String &p_error, bool p_editor_notify, ErrorHandlerType p_type) {
	constexpr int kBacktraceDepth = 25;
	void *backtrace_addrs[kBacktraceDepth];
	Dl_info info;

	int trace_size = backtrace(backtrace_addrs, kBacktraceDepth);

	for (int i = 0; i < trace_size; ++i) {
		dladdr(backtrace_addrs[i], &info);

		FunctionInfo func_info = describe_function(info, backtrace_addrs[i]);
		_err_print_error(func_info.function, func_info.file, func_info.line, "", func_info.descriptor + p_error, p_editor_notify, p_type);
	}
}

void _err_print_error_backtrace(const char *filter, const String &p_error, bool p_editor_notify, ErrorHandlerType p_type) {
	FunctionInfo info = calling_function(filter, 1);
	_err_print_error(info.function, info.file, info.line, "", info.descriptor + p_error, p_editor_notify, p_type);
}

void _err_flush_stdout() {
	fflush(stdout);
}
