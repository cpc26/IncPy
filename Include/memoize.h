/* The rest of the interpreter should only call functions exported by
   this header file.  These functions are the link between IncPy and the
   rest of the interpreter.

   IncPy: An auto-memoizing Python interpreter supporting incremental
   recomputation. Copyright 2009-2010 Philip J. Guo (pg@cs.stanford.edu)
   All rights reserved.

   This code carries the same license as the enclosing Python
   distribution: http://www.python.org/psf/license/ 

*/


#ifndef Py_MEMOIZE_H
#define Py_MEMOIZE_H
#ifdef __cplusplus
extern "C" {
#endif

#include "Python.h"
#include "frameobject.h"


#define PYPRINT(obj) do {PyObject_Print(obj, stdout, 0); printf("\n");} while(0)


// use *_CheckExact for speed ...

// ORDER MATTERS for short-circuiting ... put the most commonly-occuring
// ones first ...
#define IS_PRIMITIVE_TYPE(obj) \
  ((obj == Py_None) || \
   PyString_CheckExact(obj) || \
   PyInt_CheckExact(obj) || \
   PyLong_CheckExact(obj) || \
   PyBool_Check(obj) || \
   PyComplex_CheckExact(obj) || \
   PyFloat_CheckExact(obj) || \
   PyUnicode_CheckExact(obj))


// don't try to pickle values of these types for return values and
// global vars since it's hard to restore their state on a future
// execution
//
// ORDER MATTERS for short-circuiting ... put the most commonly-occuring
// ones first ...
#define NEVER_PICKLE(val) \
  (PyModule_CheckExact(val) || \
   PyFunction_Check(val) || \
   PyCFunction_Check(val) || \
   PyMethod_Check(val) || \
   PyType_CheckExact(val) || \
   PyClass_Check(val) || \
   PyFile_CheckExact(val) || \
   (strcmp(Py_TYPE(val)->tp_name, "sqlite3.Cursor") == 0))


// initialize in pg_initialize(), destroy in pg_finalize()
PyObject* all_func_memo_info_dict;
PyObject* func_name_to_code_dependency;

PyObject* cPickle_dumpstr_func;
PyObject* cPickle_dump_func;
PyObject* cPickle_load_func;

PyObject* hexdigest_str(PyObject* s);

int obj_equals(PyObject* obj1, PyObject* obj2);

void add_new_code_dep(PyCodeObject* cod);

// hook from PyCode_New()
void pg_init_new_code_object(PyCodeObject* co);

// handler for PyFunction_New()
void pg_CREATE_FUNCTION_event(PyFunctionObject* func);

void pg_initialize(void);
void pg_finalize(void);

PyObject* pg_enter_frame(PyFrameObject* f);
void pg_exit_frame(PyFrameObject* f, PyObject* retval);

// handler for LOAD_GLOBAL(varname) --> value
void pg_LOAD_GLOBAL_event(PyObject *varname, PyObject *value);
// handler for LOAD(object.attrname) --> value
void pg_GetAttr_event(PyObject *object, PyObject *attrname, PyObject *value);

// handler for any actions that extend global reachability from parent
// to child (e.g., child = parent[index])
void pg_extend_reachability_event(PyObject* parent, PyObject* child);

// handler for STORE_GLOBAL(varname) or DELETE_GLOBA(varname)
void pg_STORE_DEL_GLOBAL_event(PyObject *varname);

// called whenever object is ABOUT TO BE mutated by either storing or
// deleting one of its attributes (e.g., in an instance) or items (e.g.,
// in a sequence)
void pg_about_to_MUTATE_event(PyObject *object);

// handler for BUILD_CLASS opcode
void pg_BUILD_CLASS_event(PyObject* name, PyObject* methods_dict);

void pg_FILE_OPEN_event(PyFileObject* fobj);
void pg_FILE_CLOSE_event(PyFileObject* fobj);

void pg_FILE_READ_event(PyFileObject* fobj);


// when you're calling a method implemented in C with the name func_name
// and the self object 'self' ... e.g.,
// lst = [1,2,3]
// lst.append(4)
// (here, func_name is "append" and self is the list object [1,2,3])
void pg_about_to_CALL_C_METHOD_WITH_SELF_event(char* func_name, PyObject* self);


// handlers for file write operations as defined in Objects/fileobject.c:
void pg_intercept_PyFile_WriteString(const char *s, PyObject *f);
void pg_intercept_PyFile_WriteObject(PyObject *v, PyObject *f, int flags);
void pg_intercept_PyFile_SoftSpace(PyObject *f, int newflag);
void pg_intercept_file_write(PyFileObject *f, PyObject *args);
void pg_intercept_file_writelines(PyFileObject *f, PyObject *args);
void pg_intercept_file_writelines(PyFileObject *f, PyObject *seq);
void pg_intercept_file_truncate(PyFileObject *f, PyObject *args);

// intercept in Objects/codeobject.c:PyCode_New()
int pg_ignore_code(PyCodeObject* co);
PyObject* pg_create_canonical_code_name(PyCodeObject* co);

PyObject* canonical_name_to_filename(PyObject* func_name);

/* check for word size

   stolen from Valgrind: valgrind-3.5.0/VEX/pub/libvex_basictypes.h */

/* The following 4 work OK for Linux. */
#if defined(__x86_64__)
#   define HOST_WORDSIZE 8
#elif defined(__i386__)
#   define HOST_WORDSIZE 4
#elif defined(__powerpc__) && defined(__powerpc64__)
#   define HOST_WORDSIZE 8
#elif defined(__powerpc__) && !defined(__powerpc64__)
#   define HOST_WORDSIZE 4

#elif defined(_AIX) && !defined(__64BIT__)
#   define HOST_WORDSIZE 4
#elif defined(_AIX) && defined(__64BIT__)
#   define HOST_WORDSIZE 8

#else
#   error "IncPy compilation error: Can't establish the host architecture"
#endif

#if HOST_WORDSIZE == 8
#   define HOST_IS_64BIT
#endif

// check their sizes in pg_initialize():
typedef unsigned char           UInt8;
typedef unsigned short          UInt16;
typedef unsigned int            UInt32;
typedef unsigned long long int  UInt64;


/* Maintain shadow metadata for each object.  We do this 'shadowing'
   rather than DIRECTLY augmenting the PyObject struct so that we can
   maintain BINARY COMPATIBILITY with existing libraries containing
   compiled extension code (e.g., numpy, scipy).  Pre-compiled versions
   of these libraries are hard-coded with the default PyObject struct
   layout, so if we change PyObject by adding fields to it, then we will
   need to re-compile those libraries, which is a big pain! */

/* efficient multi-level mapping of PyObject addresses to metadata
   (inspired by Valgrind Memcheck's multi-level shadow memory
    implementation: http://valgrind.org/docs/shadow-memory2007.pdf) */

// TODO: implement garbage collection for shadow memory pages
//       if it makes sense to do so

#define METADATA_MAP_SIZE 65536 // 16 bits
#define METADATA_MAP_MASK (METADATA_MAP_SIZE-1)


typedef struct {
  /* WEAK REFERENCE - This should only be set for MUTABLE values
     (see update_global_container_weakref() for more details on why)

     since this is a weak reference, make sure that there's at least
     ONE other reference to this object, so that it doesn't get
     garbage collected */
  PyObject* global_container_weakref;

  /* if this object is MUTABLE and reachable from an argument of a
     function invocation, then set this value to the
     start_func_call_time field of the function's PyFrameObject
     (measured in units of num_executed_func_calls).

     subtle: if it's reachable from an argument of MORE THAN one
     function on the stack, then set this value to the
     start_func_call_time of the outer-most function on the stack ...
     e.g., if foo(x) calls bar(x), then the
     arg_reachable_func_start_time for x should be set to the
     start_func_call_time of foo, NOT bar, since foo is farther
     'outwards' than bar */
  unsigned int arg_reachable_func_start_time;
} obj_metadata;

#ifdef HOST_IS_64BIT
// 64-bit architecture

obj_metadata**** level_1_map;

/*

level_1_map - allocate to 65536 obj_metadata*** elements,     address with obj[63:48]
level_2_map - lazy-allocate to 65536 obj_metadata** elements, address with obj[47:32]
level_3_map - lazy-allocate to 65536 obj_metadata* elements,  address with obj[31:16]
level_4_map - lazy-allocate to 65536 obj_metadata elements,   address with obj[15:0]

*/

#else
// 32-bit architecture

obj_metadata** level_1_map;

/*

level_1_map - allocate to 65536 obj_metadata* elements,     address with obj[31:16]
level_2_map - lazy-allocate to 65536 obj_metadata elements, address with obj[15:0]

*/
#endif // HOST_IS_64BIT

void set_global_container(PyObject* obj, PyObject* global_container);
PyObject* get_global_container(PyObject* obj);

void set_arg_reachable_func_start_time(PyObject* obj, unsigned int start_func_call_time);
unsigned int get_arg_reachable_func_start_time(PyObject* obj);


#ifdef __cplusplus
}
#endif
#endif /* !Py_MEMOIZE_H */

