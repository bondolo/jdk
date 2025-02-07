/*
 * Copyright (c) 1998, 2022, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_RUNTIME_SYNCHRONIZER_HPP
#define SHARE_RUNTIME_SYNCHRONIZER_HPP

#include "memory/padded.hpp"
#include "oops/markWord.hpp"
#include "runtime/basicLock.hpp"
#include "runtime/handles.hpp"
#include "utilities/resourceHash.hpp"

template <typename T> class GrowableArray;
class LogStream;
class ObjectMonitor;
class ThreadsList;

// Hash table of void* to a list of ObjectMonitor* owned by the JavaThread.
// The JavaThread's owner key is either a JavaThread* or a stack lock
// address in the JavaThread so we use "void*".
//
class ObjectMonitorsHashtable {
 private:
  static unsigned int ptr_hash(void* const& s1) {
    // 2654435761 = 2^32 * Phi (golden ratio)
    return (unsigned int)(((uint32_t)(uintptr_t)s1) * 2654435761u);
  }

 public:
  class PtrList;

 private:
  // ResourceHashtable SIZE is specified at compile time so we
  // use 1031 which is the first prime after 1024.
  typedef ResourceHashtable<void*, PtrList*, 1031, ResourceObj::C_HEAP, mtThread,
                            &ObjectMonitorsHashtable::ptr_hash> PtrTable;
  PtrTable* _ptrs;
  size_t _key_count;
  size_t _om_count;

 public:
  // ResourceHashtable is passed to various functions and populated in
  // different places so we allocate it using C_HEAP to make it immune
  // from any ResourceMarks that happen to be in the code paths.
  ObjectMonitorsHashtable() : _ptrs(new (ResourceObj::C_HEAP, mtThread) PtrTable), _key_count(0), _om_count(0) {}

  ~ObjectMonitorsHashtable();

  void add_entry(void* key, ObjectMonitor* om);

  void add_entry(void* key, PtrList* list) {
    _ptrs->put(key, list);
    _key_count++;
  }

  PtrList* get_entry(void* key) {
    PtrList** listpp = _ptrs->get(key);
    return (listpp == nullptr) ? nullptr : *listpp;
  }

  bool has_entry(void* key) {
    PtrList** listpp = _ptrs->get(key);
    return listpp != nullptr && *listpp != nullptr;
  }

  bool has_entry(void* key, ObjectMonitor* om);

  size_t key_count() { return _key_count; }
  size_t om_count() { return _om_count; }
};

class MonitorList {
  friend class VMStructs;

private:
  ObjectMonitor* volatile _head;
  volatile size_t _count;
  volatile size_t _max;

public:
  void add(ObjectMonitor* monitor);
  size_t unlink_deflated(Thread* current, LogStream* ls, elapsedTimer* timer_p,
                         GrowableArray<ObjectMonitor*>* unlinked_list);
  size_t count() const;
  size_t max() const;

  class Iterator;
  Iterator iterator() const;
};

class MonitorList::Iterator {
  ObjectMonitor* _current;

public:
  Iterator(ObjectMonitor* head) : _current(head) {}
  bool has_next() const { return _current != NULL; }
  ObjectMonitor* next();
};

class ObjectSynchronizer : AllStatic {
  friend class VMStructs;

 public:
  typedef enum {
    inflate_cause_vm_internal = 0,
    inflate_cause_monitor_enter = 1,
    inflate_cause_wait = 2,
    inflate_cause_notify = 3,
    inflate_cause_hash_code = 4,
    inflate_cause_jni_enter = 5,
    inflate_cause_jni_exit = 6,
    inflate_cause_nof = 7 // Number of causes
  } InflateCause;

  typedef enum {
    NOT_ENABLED    = 0,
    FATAL_EXIT     = 1,
    LOG_WARNING    = 2
  } SyncDiagnosticOption;

  // exit must be implemented non-blocking, since the compiler cannot easily handle
  // deoptimization at monitor exit. Hence, it does not take a Handle argument.

  // This is the "slow path" version of monitor enter and exit.
  static void enter(Handle obj, BasicLock* lock, JavaThread* current);
  static void exit(oop obj, BasicLock* lock, JavaThread* current);

  // Used only to handle jni locks or other unmatched monitor enter/exit
  // Internally they will use heavy weight monitor.
  static void jni_enter(Handle obj, JavaThread* current);
  static void jni_exit(oop obj, TRAPS);

  // Handle all interpreter, compiler and jni cases
  static int  wait(Handle obj, jlong millis, TRAPS);
  static void notify(Handle obj, TRAPS);
  static void notifyall(Handle obj, TRAPS);

  static bool quick_notify(oopDesc* obj, JavaThread* current, bool All);
  static bool quick_enter(oop obj, JavaThread* current, BasicLock* Lock);

  // Special internal-use-only method for use by JVM infrastructure
  // that needs to wait() on a java-level object but must not respond
  // to interrupt requests and doesn't timeout.
  static void wait_uninterruptibly(Handle obj, JavaThread* current);

  // used by classloading to free classloader object lock,
  // wait on an internal lock, and reclaim original lock
  // with original recursion count
  static intx complete_exit(Handle obj, JavaThread* current);
  static void reenter (Handle obj, intx recursions, JavaThread* current);

  // Inflate light weight monitor to heavy weight monitor
  static ObjectMonitor* inflate(Thread* current, oop obj, const InflateCause cause);
  // This version is only for internal use
  static void inflate_helper(oop obj);
  static const char* inflate_cause_name(const InflateCause cause);

  // Returns the identity hash value for an oop
  // NOTE: It may cause monitor inflation
  static intptr_t FastHashCode(Thread* current, oop obj);

  // java.lang.Thread support
  static bool current_thread_holds_lock(JavaThread* current, Handle h_obj);

  static JavaThread* get_lock_owner(ThreadsList * t_list, Handle h_obj);

  // JNI detach support
  static void release_monitors_owned_by_thread(JavaThread* current);

  // Iterate ObjectMonitors where the owner == thread; this does NOT include
  // ObjectMonitors where owner is set to a stack lock address in thread:
  //
  // This version of monitors_iterate() works with the in-use monitor list.
  static void monitors_iterate(MonitorClosure* m, JavaThread* thread);
  // This version of monitors_iterate() works with the specified linked list.
  static void monitors_iterate(MonitorClosure* closure,
                               ObjectMonitorsHashtable::PtrList* list,
                               JavaThread* thread);

  // Initialize the gInflationLocks
  static void initialize();

  // GC: we currently use aggressive monitor deflation policy
  // Basically we try to deflate all monitors that are not busy.
  static size_t deflate_idle_monitors(ObjectMonitorsHashtable* table);

  // Deflate idle monitors:
  static void chk_for_block_req(JavaThread* current, const char* op_name,
                                const char* cnt_name, size_t cnt, LogStream* ls,
                                elapsedTimer* timer_p);
  static size_t deflate_monitor_list(Thread* current, LogStream* ls, elapsedTimer* timer_p,
                                     ObjectMonitorsHashtable* table);
  static size_t in_use_list_ceiling();
  static void dec_in_use_list_ceiling();
  static void inc_in_use_list_ceiling();
  static void set_in_use_list_ceiling(size_t new_value);
  static bool is_async_deflation_needed();
  static bool is_async_deflation_requested() { return _is_async_deflation_requested; }
  static bool is_final_audit() { return _is_final_audit; }
  static void set_is_final_audit() { _is_final_audit = true; }
  static jlong last_async_deflation_time_ns() { return _last_async_deflation_time_ns; }
  static bool request_deflate_idle_monitors();  // for whitebox test support
  static void set_is_async_deflation_requested(bool new_value) { _is_async_deflation_requested = new_value; }
  static jlong time_since_last_async_deflation_ms();

  // debugging
  static void audit_and_print_stats(bool on_exit);
  static void chk_in_use_list(outputStream* out, int* error_cnt_p);
  static void chk_in_use_entry(ObjectMonitor* n, outputStream* out,
                               int* error_cnt_p);
  static void do_final_audit_and_print_stats();
  static void log_in_use_monitor_details(outputStream* out);

 private:
  friend class SynchronizerTest;

  static MonitorList _in_use_list;
  static volatile bool _is_async_deflation_requested;
  static volatile bool _is_final_audit;
  static jlong         _last_async_deflation_time_ns;

  // Support for SynchronizerTest access to GVars fields:
  static u_char* get_gvars_addr();
  static u_char* get_gvars_hc_sequence_addr();
  static size_t get_gvars_size();
  static u_char* get_gvars_stw_random_addr();

  static void handle_sync_on_value_based_class(Handle obj, JavaThread* current);
};

// ObjectLocker enforces balanced locking and can never throw an
// IllegalMonitorStateException. However, a pending exception may
// have to pass through, and we must also be able to deal with
// asynchronous exceptions. The caller is responsible for checking
// the thread's pending exception if needed.
class ObjectLocker : public StackObj {
 private:
  JavaThread* _thread;
  Handle      _obj;
  BasicLock   _lock;
 public:
  ObjectLocker(Handle obj, JavaThread* current);
  ~ObjectLocker();

  // Monitor behavior
  void wait(TRAPS)  { ObjectSynchronizer::wait(_obj, 0, CHECK); } // wait forever
  void notify_all(TRAPS)  { ObjectSynchronizer::notifyall(_obj, CHECK); }
  void wait_uninterruptibly(JavaThread* current) { ObjectSynchronizer::wait_uninterruptibly(_obj, current); }
};

#endif // SHARE_RUNTIME_SYNCHRONIZER_HPP
