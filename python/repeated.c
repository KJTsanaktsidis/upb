/*
 * Copyright (c) 2009-2021, Google LLC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Google LLC nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL Google LLC BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "python/repeated.h"

#include "python/convert.h"
#include "python/message.h"
#include "python/protobuf.h"

static PyObject* PyUpb_RepeatedCompositeContainer_Append(PyObject* _self,
                                                         PyObject* value);
static PyObject* PyUpb_RepeatedScalarContainer_Append(PyObject* _self,
                                                      PyObject* value);

// For an expression like:
//    foo[index]
//
// Converts `index` to an effective i/count/step, for a repeated field
// field of size `size`.
static bool IndexToRange(PyObject* index, Py_ssize_t size, Py_ssize_t* i,
                         Py_ssize_t* count, Py_ssize_t* step) {
  assert(i && count && step);
  if (PySlice_Check(index)) {
    Py_ssize_t start, stop;
    if (PySlice_Unpack(index, &start, &stop, step) < 0) return false;
    *count = PySlice_AdjustIndices(size, &start, &stop, *step);
    *i = start;
  } else {
    *i = PyNumber_AsSsize_t(index, PyExc_IndexError);

    if (*i == -1 && PyErr_Occurred()) {
      PyErr_SetString(PyExc_TypeError, "list indices must be integers");
      return false;
    }

    if (*i < 0) *i += size;
    *step = 0;
    *count = 1;

    if (*i < 0 || size <= *i) {
      PyErr_Format(PyExc_IndexError, "list index out of range");
      return false;
    }
  }
  return true;
}

// Wrapper for a repeated field.
typedef struct {
  PyObject_HEAD
  PyObject* arena;
  // The field descriptor (PyObject*).
  // The low bit indicates whether the container is reified (see ptr below).
  //   - low bit set: repeated field is a stub (no underlying data).
  //   - low bit clear: repeated field is reified (points to upb_array).
  uintptr_t field;
  union {
    PyObject* parent;  // stub: owning pointer to parent message.
    upb_array* arr;    // reified: the data for this array.
  } ptr;
} PyUpb_RepeatedContainer;

static bool PyUpb_RepeatedContainer_IsStub(PyUpb_RepeatedContainer* self) {
  return self->field & 1;
}

static PyObject* PyUpb_RepeatedContainer_GetFieldDescriptor(
    PyUpb_RepeatedContainer* self) {
  return (PyObject*)(self->field & ~(uintptr_t)1);
}

static const upb_fielddef* PyUpb_RepeatedContainer_GetField(
    PyUpb_RepeatedContainer* self) {
  return PyUpb_FieldDescriptor_GetDef(
      PyUpb_RepeatedContainer_GetFieldDescriptor(self));
}

// If the repeated field is reified, returns it.  Otherwise, returns NULL.
// If NULL is returned, the object is empty and has no underlying data.
static upb_array* PyUpb_RepeatedContainer_GetIfReified(
    PyUpb_RepeatedContainer* self) {
  return PyUpb_RepeatedContainer_IsStub(self) ? NULL : self->ptr.arr;
}

void PyUpb_RepeatedContainer_Reify(PyObject* _self, upb_array* arr) {
  PyUpb_RepeatedContainer* self = (PyUpb_RepeatedContainer*)_self;
  assert(PyUpb_RepeatedContainer_IsStub(self));
  PyUpb_ObjCache_Add(arr, &self->ob_base);
  Py_DECREF(self->ptr.parent);
  self->ptr.arr = arr;  // Overwrites self->ptr.parent.
  self->field &= ~(uintptr_t)1;
  assert(!PyUpb_RepeatedContainer_IsStub(self));
}

static upb_array* PyUpb_RepeatedContainer_AssureReified(
    PyUpb_RepeatedContainer* self) {
  upb_array* arr = PyUpb_RepeatedContainer_GetIfReified(self);
  if (arr) return arr;  // Already writable.

  const upb_fielddef* f = PyUpb_RepeatedContainer_GetField(self);
  upb_arena* arena = PyUpb_Arena_Get(self->arena);
  arr = upb_array_new(arena, upb_fielddef_type(f));
  PyUpb_CMessage_SetConcreteSubobj(self->ptr.parent, f,
                                   (upb_msgval){.array_val = arr});
  PyUpb_RepeatedContainer_Reify((PyObject*)self, arr);
  return arr;
}

static void PyUpb_RepeatedContainer_Dealloc(PyObject* _self) {
  PyUpb_RepeatedContainer* self = (PyUpb_RepeatedContainer*)_self;
  Py_DECREF(self->arena);
  if (PyUpb_RepeatedContainer_IsStub(self)) {
    PyUpb_CMessage_CacheDelete(self->ptr.parent,
                               PyUpb_RepeatedContainer_GetField(self));
    Py_DECREF(self->ptr.parent);
  } else {
    PyUpb_ObjCache_Delete(self->ptr.arr);
  }
  Py_DECREF(PyUpb_RepeatedContainer_GetFieldDescriptor(self));
  PyUpb_Dealloc(self);
}

static PyTypeObject* PyUpb_RepeatedContainer_GetClass(const upb_fielddef* f) {
  assert(upb_fielddef_isseq(f) && !upb_fielddef_ismap(f));
  PyUpb_ModuleState* state = PyUpb_ModuleState_Get();
  return upb_fielddef_issubmsg(f) ? state->repeated_composite_container_type
                                  : state->repeated_scalar_container_type;
}

static Py_ssize_t PyUpb_RepeatedContainer_Length(PyObject* self) {
  upb_array* arr =
      PyUpb_RepeatedContainer_GetIfReified((PyUpb_RepeatedContainer*)self);
  return arr ? upb_array_size(arr) : 0;
}

PyObject* PyUpb_RepeatedContainer_NewStub(PyObject* parent,
                                          const upb_fielddef* f,
                                          PyObject* arena) {
  PyTypeObject* cls = PyUpb_RepeatedContainer_GetClass(f);
  PyUpb_RepeatedContainer* repeated = (void*)PyType_GenericAlloc(cls, 0);
  repeated->arena = arena;
  repeated->field = (uintptr_t)PyUpb_FieldDescriptor_Get(f) | 1;
  repeated->ptr.parent = parent;
  Py_INCREF(arena);
  Py_INCREF(parent);
  return &repeated->ob_base;
}

PyObject* PyUpb_RepeatedContainer_GetOrCreateWrapper(upb_array* arr,
                                                     const upb_fielddef* f,
                                                     PyObject* arena) {
  PyObject* ret = PyUpb_ObjCache_Get(arr);
  if (ret) return ret;

  PyTypeObject* cls = PyUpb_RepeatedContainer_GetClass(f);
  PyUpb_RepeatedContainer* repeated = (void*)PyType_GenericAlloc(cls, 0);
  repeated->arena = arena;
  repeated->field = (uintptr_t)PyUpb_FieldDescriptor_Get(f);
  repeated->ptr.arr = arr;
  ret = &repeated->ob_base;
  Py_INCREF(arena);
  PyUpb_ObjCache_Add(arr, ret);
  return ret;
}

PyObject* PyUpb_RepeatedContainer_Extend(PyObject* _self, PyObject* value) {
  PyUpb_RepeatedContainer* self = (PyUpb_RepeatedContainer*)_self;
  upb_array* arr = PyUpb_RepeatedContainer_AssureReified(self);
  size_t start_size = upb_array_size(arr);
  PyObject* it = PyObject_GetIter(value);
  if (!it) {
    PyErr_SetString(PyExc_TypeError, "Value must be iterable");
    return NULL;
  }

  const upb_fielddef* f = PyUpb_RepeatedContainer_GetField(self);
  bool submsg = upb_fielddef_issubmsg(f);
  PyObject* e;

  while ((e = PyIter_Next(it))) {
    PyObject* ret;
    if (submsg) {
      ret = PyUpb_RepeatedCompositeContainer_Append(_self, e);
    } else {
      ret = PyUpb_RepeatedScalarContainer_Append(_self, e);
    }
    Py_XDECREF(ret);
    Py_DECREF(e);
  }

  Py_DECREF(it);

  if (PyErr_Occurred()) {
    upb_array_resize(arr, start_size, NULL);
    return NULL;
  }

  Py_RETURN_NONE;
}

static PyObject* PyUpb_RepeatedContainer_Item(PyObject* _self,
                                              Py_ssize_t index) {
  PyUpb_RepeatedContainer* self = (PyUpb_RepeatedContainer*)_self;
  upb_array* arr = PyUpb_RepeatedContainer_GetIfReified(self);
  Py_ssize_t size = arr ? upb_array_size(arr) : 0;
  if (index < 0 || index >= size) {
    PyErr_Format(PyExc_IndexError, "list index (%zd) out of range", index);
    return NULL;
  }
  const upb_fielddef* f = PyUpb_RepeatedContainer_GetField(self);
  return PyUpb_UpbToPy(upb_array_get(arr, index), f, self->arena);
}

PyObject* PyUpb_RepeatedContainer_ToList(PyObject* _self) {
  PyUpb_RepeatedContainer* self = (PyUpb_RepeatedContainer*)_self;
  upb_array* arr = PyUpb_RepeatedContainer_GetIfReified(self);
  if (!arr) return PyList_New(0);

  const upb_fielddef* f = PyUpb_RepeatedContainer_GetField(self);
  size_t n = upb_array_size(arr);
  PyObject* list = PyList_New(n);
  for (size_t i = 0; i < n; i++) {
    PyObject* val = PyUpb_UpbToPy(upb_array_get(arr, i), f, self->arena);
    if (!val) {
      Py_DECREF(list);
      return NULL;
    }
    PyList_SetItem(list, i, val);
  }
  return list;
}

static PyObject* PyUpb_RepeatedContainer_Repr(PyObject* _self) {
  PyObject* list = PyUpb_RepeatedContainer_ToList(_self);
  if (!list) return NULL;
  assert(!PyErr_Occurred());
  PyObject* repr = PyObject_Repr(list);
  Py_DECREF(list);
  return repr;
}

static PyObject* PyUpb_RepeatedContainer_RichCompare(PyObject* _self,
                                                     PyObject* _other,
                                                     int opid) {
  if (opid != Py_EQ && opid != Py_NE) {
    Py_INCREF(Py_NotImplemented);
    return Py_NotImplemented;
  }
  PyObject* list1 = PyUpb_RepeatedContainer_ToList(_self);
  PyObject* list2 = _other;
  PyObject* del = NULL;
  if (PyObject_TypeCheck(_other, _self->ob_type)) {
    del = list2 = PyUpb_RepeatedContainer_ToList(_other);
  }
  PyObject* ret = PyObject_RichCompare(list1, list2, opid);
  Py_DECREF(list1);
  Py_XDECREF(del);
  return ret;
}

static PyObject* PyUpb_RepeatedContainer_Subscript(PyObject* _self,
                                                   PyObject* key) {
  PyUpb_RepeatedContainer* self = (PyUpb_RepeatedContainer*)_self;
  upb_array* arr = PyUpb_RepeatedContainer_GetIfReified(self);
  Py_ssize_t size = arr ? upb_array_size(arr) : 0;
  Py_ssize_t idx, count, step;
  if (!IndexToRange(key, size, &idx, &count, &step)) return NULL;
  const upb_fielddef* f = PyUpb_RepeatedContainer_GetField(self);
  if (step == 0) {
    return PyUpb_UpbToPy(upb_array_get(arr, idx), f, self->arena);
  } else {
    PyObject* list = PyList_New(count);
    for (Py_ssize_t i = 0; i < count; i++, idx += step) {
      upb_msgval msgval = upb_array_get(self->ptr.arr, idx);
      PyObject* item = PyUpb_UpbToPy(msgval, f, self->arena);
      if (!item) {
        Py_DECREF(list);
        return NULL;
      }
      PyList_SetItem(list, i, item);
    }
    return list;
  }
}

static int PyUpb_RepeatedContainer_SetSubscript(
    PyUpb_RepeatedContainer* self, upb_array* arr, const upb_fielddef* f,
    Py_ssize_t idx, Py_ssize_t count, Py_ssize_t step, PyObject* value) {
  upb_arena* arena = PyUpb_Arena_Get(self->arena);
  if (upb_fielddef_issubmsg(f)) {
    PyErr_SetString(PyExc_TypeError, "does not support assignment");
    return -1;
  }

  if (step == 0) {
    // Set single value.
    upb_msgval msgval;
    if (!PyUpb_PyToUpb(value, f, &msgval, arena)) return -1;
    upb_array_set(arr, idx, msgval);
    return 0;
  }

  // Set range.
  PyObject* seq =
      PySequence_Fast(value, "must assign iterable to extended slice");
  PyObject* item = NULL;
  int ret = -1;
  if (!seq) goto err;
  if (PySequence_Size(seq) != count) {
    PyErr_Format(PyExc_ValueError,
                  "attempt to assign sequence of size %zd to extended slice "
                  "of size %zd",
                  PySequence_Size(seq), count);
    goto err;
  }
  for (Py_ssize_t i = 0; i < count; i++, idx += step) {
    upb_msgval msgval;
    item = PySequence_GetItem(seq, i);
    if (!item) goto err;
    // XXX: if this fails we can leave the list partially mutated.
    if (!PyUpb_PyToUpb(item, f, &msgval, arena)) goto err;
    Py_DECREF(item);
    item = NULL;
    upb_array_set(arr, idx, msgval);
  }
  ret = 0;

err:
  Py_XDECREF(seq);
  Py_XDECREF(item);
  return ret;
}

static int PyUpb_RepeatedContainer_DeleteSubscript(upb_array* arr,
                                                   Py_ssize_t idx,
                                                   Py_ssize_t count,
                                                   Py_ssize_t step) {
  // Normalize direction: deletion is order-independent.
  Py_ssize_t start = idx;
  if (step < 0) {
    Py_ssize_t end = start + step * (count - 1);
    start = end;
    step = -step;
  }

  size_t dst = start;
  size_t src;
  if (step > 1) {
    // Move elements between steps:
    //
    //        src
    //         |
    // |------X---X---X---X------------------------------|
    //        |
    //       dst           <-------- tail -------------->
    src = start + 1;
    for (Py_ssize_t i = 1; i < count; i++, dst += step - 1, src += step) {
      upb_array_move(arr, dst, src, step);
    }
  } else {
    src = start + count;
  }

  // Move tail.
  size_t tail = upb_array_size(arr) - src;
  size_t new_size = dst + tail;
  assert(new_size == upb_array_size(arr) - count);
  upb_array_move(arr, dst, src, tail);
  upb_array_resize(arr, new_size, NULL);
  return 0;
}

static int PyUpb_RepeatedContainer_AssignSubscript(PyObject* _self,
                                                   PyObject* key,
                                                   PyObject* value) {
  PyUpb_RepeatedContainer* self = (PyUpb_RepeatedContainer*)_self;
  const upb_fielddef* f = PyUpb_RepeatedContainer_GetField(self);
  upb_array* arr = PyUpb_RepeatedContainer_GetIfReified(self);
  Py_ssize_t size = arr ? upb_array_size(arr) : 0;
  Py_ssize_t idx, count, step;
  if (!IndexToRange(key, size, &idx, &count, &step)) return -1;
  if (value) {
    return PyUpb_RepeatedContainer_SetSubscript(self, arr, f, idx, count, step,
                                                value);
  } else {
    return PyUpb_RepeatedContainer_DeleteSubscript(arr, idx, count, step);
  }
}

static PyObject* PyUpb_RepeatedContainer_Pop(PyObject* _self, PyObject* args) {
  PyUpb_RepeatedContainer* self = (PyUpb_RepeatedContainer*)_self;
  Py_ssize_t index = -1;
  if (!PyArg_ParseTuple(args, "|n", &index)) return NULL;
  upb_array* arr = PyUpb_RepeatedContainer_AssureReified(self);
  size_t size = upb_array_size(arr);
  if (index < 0) index += size;
  if (index >= size) index = size - 1;
  PyObject* ret = PyUpb_RepeatedContainer_Item(_self, index);
  if (!ret) return NULL;
  upb_array_delete(self->ptr.arr, index, 1);
  return ret;
}

// A helper function used only for Sort().
static bool PyUpb_RepeatedContainer_Assign(PyObject* _self, PyObject* list) {
  PyUpb_RepeatedContainer* self = (PyUpb_RepeatedContainer*)_self;
  const upb_fielddef* f = PyUpb_RepeatedContainer_GetField(self);
  upb_array* arr = PyUpb_RepeatedContainer_AssureReified(self);
  Py_ssize_t size = PyList_Size(list);
  bool submsg = upb_fielddef_issubmsg(f);
  upb_arena* arena = PyUpb_Arena_Get(self->arena);
  for (Py_ssize_t i = 0; i < size; ++i) {
    PyObject* obj = PyList_GetItem(list, i);
    upb_msgval msgval;
    if (submsg) {
      msgval.msg_val = PyUpb_CMessage_GetIfReified(obj);
      assert(msgval.msg_val);
    } else {
      if (!PyUpb_PyToUpb(obj, f, &msgval, arena)) return false;
    }
    upb_array_set(arr, i, msgval);
  }
  return true;
}

static PyObject* PyUpb_RepeatedContainer_Sort(PyObject* pself, PyObject* args,
                                              PyObject* kwds) {
  // Support the old sort_function argument for backwards
  // compatibility.
  if (kwds != NULL) {
    PyObject* sort_func = PyDict_GetItemString(kwds, "sort_function");
    if (sort_func != NULL) {
      // Must set before deleting as sort_func is a borrowed reference
      // and kwds might be the only thing keeping it alive.
      if (PyDict_SetItemString(kwds, "cmp", sort_func) == -1) return NULL;
      if (PyDict_DelItemString(kwds, "sort_function") == -1) return NULL;
    }
  }

  PyObject* ret = NULL;
  PyObject* full_slice = NULL;
  PyObject* list = NULL;
  PyObject* m = NULL;
  PyObject* res = NULL;
  if ((full_slice = PySlice_New(NULL, NULL, NULL)) &&
      (list = PyUpb_RepeatedContainer_Subscript(pself, full_slice)) &&
      (m = PyObject_GetAttrString(list, "sort")) &&
      (res = PyObject_Call(m, args, kwds)) &&
      PyUpb_RepeatedContainer_Assign(pself, list)) {
    Py_INCREF(Py_None);
    ret = Py_None;
  }

  Py_XDECREF(full_slice);
  Py_XDECREF(list);
  Py_XDECREF(m);
  Py_XDECREF(res);
  return ret;
}

static PyObject* PyUpb_RepeatedContainer_MergeFrom(PyObject* _self,
                                                   PyObject* args) {
  return PyUpb_RepeatedContainer_Extend(_self, args);
}

// -----------------------------------------------------------------------------
// RepeatedCompositeContainer
// -----------------------------------------------------------------------------

static PyObject* PyUpb_RepeatedCompositeContainer_AppendNew(PyObject* _self) {
  PyUpb_RepeatedContainer* self = (PyUpb_RepeatedContainer*)_self;
  upb_array* arr = PyUpb_RepeatedContainer_AssureReified(self);
  if (!arr) return NULL;
  const upb_fielddef* f = PyUpb_RepeatedContainer_GetField(self);
  upb_arena* arena = PyUpb_Arena_Get(self->arena);
  const upb_msgdef* m = upb_fielddef_msgsubdef(f);
  upb_msg* msg = upb_msg_new(m, arena);
  upb_msgval msgval = {.msg_val = msg};
  upb_array_append(arr, msgval, arena);
  return PyUpb_CMessage_Get(msg, m, self->arena);
}

PyObject* PyUpb_RepeatedCompositeContainer_Add(PyObject* _self, PyObject* args,
                                               PyObject* kwargs) {
  PyUpb_RepeatedContainer* self = (PyUpb_RepeatedContainer*)_self;
  PyObject* py_msg = PyUpb_RepeatedCompositeContainer_AppendNew(_self);
  if (!py_msg) return NULL;
  if (PyUpb_CMessage_InitAttributes(py_msg, args, kwargs) < 0) {
    Py_DECREF(py_msg);
    upb_array_delete(self->ptr.arr, upb_array_size(self->ptr.arr) - 1, 1);
    return NULL;
  }
  return py_msg;
}

static PyObject* PyUpb_RepeatedCompositeContainer_Append(PyObject* _self,
                                                         PyObject* value) {
  if (!PyUpb_CMessage_Check(value)) return NULL;
  PyObject* py_msg = PyUpb_RepeatedCompositeContainer_AppendNew(_self);
  if (!py_msg) return NULL;
  PyObject* none = PyUpb_CMessage_MergeFrom(py_msg, value);
  if (!none) {
    Py_DECREF(py_msg);
    return NULL;
  }
  Py_DECREF(none);
  return py_msg;
}

static PyObject* PyUpb_RepeatedContainer_Insert(PyObject* _self,
                                                PyObject* args) {
  PyUpb_RepeatedContainer* self = (PyUpb_RepeatedContainer*)_self;
  Py_ssize_t index;
  PyObject* value;
  if (!PyArg_ParseTuple(args, "nO", &index, &value)) return NULL;
  upb_array* arr = PyUpb_RepeatedContainer_AssureReified(self);
  if (!arr) return NULL;

  // Normalize index.
  Py_ssize_t size = upb_array_size(arr);
  if (index < 0) index += size;
  if (index < 0) index = 0;
  if (index > size) index = size;

  const upb_fielddef* f = PyUpb_RepeatedContainer_GetField(self);
  upb_msgval msgval;
  upb_arena* arena = PyUpb_Arena_Get(self->arena);
  if (upb_fielddef_issubmsg(f)) {
    // Create message.
    const upb_msgdef* m = upb_fielddef_msgsubdef(f);
    upb_msg* msg = upb_msg_new(m, arena);
    PyObject* py_msg = PyUpb_CMessage_Get(msg, m, self->arena);
    PyObject* ret = PyUpb_CMessage_MergeFrom(py_msg, value);
    Py_DECREF(py_msg);
    if (!ret) return NULL;
    Py_DECREF(ret);
    msgval.msg_val = msg;
  } else {
    if (!PyUpb_PyToUpb(value, f, &msgval, arena)) return NULL;
  }

  upb_array_insert(arr, index, 1, arena);
  upb_array_set(arr, index, msgval);

  Py_RETURN_NONE;
}

static PyMethodDef PyUpb_RepeatedCompositeContainer_Methods[] = {
    // TODO(https://github.com/protocolbuffers/upb/issues/459)
    // {"__deepcopy__", DeepCopy, METH_VARARGS, "Makes a deep copy of the class."},
    {"add", (PyCFunction)PyUpb_RepeatedCompositeContainer_Add,
     METH_VARARGS | METH_KEYWORDS, "Adds an object to the repeated container."},
    {"append", PyUpb_RepeatedCompositeContainer_Append, METH_O,
     "Appends a message to the end of the repeated container."},
    {"insert", PyUpb_RepeatedContainer_Insert, METH_VARARGS,
     "Inserts a message before the specified index."},
    {"extend", PyUpb_RepeatedContainer_Extend, METH_O,
     "Adds objects to the repeated container."},
    {"pop", PyUpb_RepeatedContainer_Pop, METH_VARARGS,
     "Removes an object from the repeated container and returns it."},
    // TODO(https://github.com/protocolbuffers/upb/issues/459)
    //{"remove", Remove, METH_O,
    // "Removes an object from the repeated container."},
    {"sort", (PyCFunction)PyUpb_RepeatedContainer_Sort,
     METH_VARARGS | METH_KEYWORDS, "Sorts the repeated container."},
    // TODO(https://github.com/protocolbuffers/upb/issues/459)
    //{"reverse", reinterpret_cast<PyCFunction>(Reverse), METH_NOARGS,
    // "Reverses elements order of the repeated container."},
    {"MergeFrom", PyUpb_RepeatedContainer_MergeFrom, METH_O,
     "Adds objects to the repeated container."},
    {NULL, NULL}};

static PyType_Slot PyUpb_RepeatedCompositeContainer_Slots[] = {
    {Py_tp_dealloc, PyUpb_RepeatedContainer_Dealloc},
    {Py_tp_methods, PyUpb_RepeatedCompositeContainer_Methods},
    {Py_sq_length, PyUpb_RepeatedContainer_Length},
    {Py_sq_item, PyUpb_RepeatedContainer_Item},
    {Py_mp_length, PyUpb_RepeatedContainer_Length},
    {Py_tp_repr, PyUpb_RepeatedContainer_Repr},
    {Py_mp_subscript, PyUpb_RepeatedContainer_Subscript},
    {Py_mp_ass_subscript, PyUpb_RepeatedContainer_AssignSubscript},
    {Py_tp_new, PyUpb_Forbidden_New},
    {Py_tp_hash, PyObject_HashNotImplemented},
    {0, NULL}};

static PyType_Spec PyUpb_RepeatedCompositeContainer_Spec = {
    PYUPB_MODULE_NAME ".RepeatedCompositeContainer",
    sizeof(PyUpb_RepeatedContainer),
    0,  // tp_itemsize
    Py_TPFLAGS_DEFAULT,
    PyUpb_RepeatedCompositeContainer_Slots,
};

// -----------------------------------------------------------------------------
// RepeatedScalarContainer
// -----------------------------------------------------------------------------

static PyObject* PyUpb_RepeatedScalarContainer_Append(PyObject* _self,
                                                      PyObject* value) {
  PyUpb_RepeatedContainer* self = (PyUpb_RepeatedContainer*)_self;
  upb_array* arr = PyUpb_RepeatedContainer_AssureReified(self);
  upb_arena* arena = PyUpb_Arena_Get(self->arena);
  const upb_fielddef* f = PyUpb_RepeatedContainer_GetField(self);
  upb_msgval msgval;
  if (!PyUpb_PyToUpb(value, f, &msgval, arena)) {
    return NULL;
  }
  upb_array_append(arr, msgval, arena);
  Py_RETURN_NONE;
}

static int PyUpb_RepeatedScalarContainer_AssignItem(PyObject* _self,
                                                    Py_ssize_t index,
                                                    PyObject* item) {
  PyUpb_RepeatedContainer* self = (PyUpb_RepeatedContainer*)_self;
  upb_array* arr = PyUpb_RepeatedContainer_GetIfReified(self);
  Py_ssize_t size = arr ? upb_array_size(arr) : 0;
  if (index < 0 || index >= size) {
    PyErr_Format(PyExc_IndexError, "list index (%zd) out of range", index);
    return -1;
  }
  const upb_fielddef* f = PyUpb_RepeatedContainer_GetField(self);
  upb_msgval msgval;
  upb_arena* arena = PyUpb_Arena_Get(self->arena);
  if (!PyUpb_PyToUpb(item, f, &msgval, arena)) {
    return -1;
  }
  upb_array_set(self->ptr.arr, index, msgval);
  return 0;
}

static PyMethodDef PyUpb_RepeatedScalarContainer_Methods[] = {
    // TODO(https://github.com/protocolbuffers/upb/issues/459)
    // {"__deepcopy__", DeepCopy, METH_VARARGS, "Makes a deep copy of the
    // class."},
    // {"__reduce__", Reduce, METH_NOARGS,
    //  "Outputs picklable representation of the repeated field."},
    {"append", PyUpb_RepeatedScalarContainer_Append, METH_O,
     "Appends an object to the repeated container."},
    {"extend", PyUpb_RepeatedContainer_Extend, METH_O,
     "Appends objects to the repeated container."},
    {"insert", PyUpb_RepeatedContainer_Insert, METH_VARARGS,
     "Inserts an object at the specified position in the container."},
    {"pop", PyUpb_RepeatedContainer_Pop, METH_VARARGS,
     "Removes an object from the repeated container and returns it."},
    // TODO(https://github.com/protocolbuffers/upb/issues/459)
    // {"remove", Remove, METH_O,
    //  "Removes an object from the repeated container."},
    {"sort", (PyCFunction)PyUpb_RepeatedContainer_Sort,
     METH_VARARGS | METH_KEYWORDS, "Sorts the repeated container."},
    // TODO(https://github.com/protocolbuffers/upb/issues/459)
    // {"reverse", reinterpret_cast<PyCFunction>(Reverse), METH_NOARGS,
    //  "Reverses elements order of the repeated container."},
    {"MergeFrom", PyUpb_RepeatedContainer_MergeFrom, METH_O,
     "Merges a repeated container into the current container."},
    {NULL, NULL}};

static PyType_Slot PyUpb_RepeatedScalarContainer_Slots[] = {
    {Py_tp_dealloc, PyUpb_RepeatedContainer_Dealloc},
    {Py_tp_methods, PyUpb_RepeatedScalarContainer_Methods},
    {Py_tp_new, PyUpb_Forbidden_New},
    {Py_tp_repr, PyUpb_RepeatedContainer_Repr},
    {Py_sq_length, PyUpb_RepeatedContainer_Length},
    {Py_sq_item, PyUpb_RepeatedContainer_Item},
    {Py_sq_ass_item, PyUpb_RepeatedScalarContainer_AssignItem},
    {Py_mp_length, PyUpb_RepeatedContainer_Length},
    {Py_mp_subscript, PyUpb_RepeatedContainer_Subscript},
    {Py_mp_ass_subscript, PyUpb_RepeatedContainer_AssignSubscript},
    {Py_tp_richcompare, PyUpb_RepeatedContainer_RichCompare},
    {Py_tp_hash, PyObject_HashNotImplemented},
    {0, NULL}};

static PyType_Spec PyUpb_RepeatedScalarContainer_Spec = {
    PYUPB_MODULE_NAME ".RepeatedScalarContainer",
    sizeof(PyUpb_RepeatedContainer),
    0,  // tp_itemsize
    Py_TPFLAGS_DEFAULT,
    PyUpb_RepeatedScalarContainer_Slots,
};

// -----------------------------------------------------------------------------
// Top Level
// -----------------------------------------------------------------------------

static bool PyUpb_Repeated_RegisterAsSequence(PyUpb_ModuleState* state) {
  PyObject* collections = NULL;
  PyObject* seq = NULL;
  PyObject* ret1 = NULL;
  PyObject* ret2 = NULL;
  PyTypeObject* type1 = state->repeated_scalar_container_type;
  PyTypeObject* type2 = state->repeated_composite_container_type;

  bool ok = (collections = PyImport_ImportModule("collections.abc")) &&
            (seq = PyObject_GetAttrString(collections, "MutableSequence")) &&
            (ret1 = PyObject_CallMethod(seq, "register", "O", type1)) &&
            (ret2 = PyObject_CallMethod(seq, "register", "O", type2));

  Py_XDECREF(collections);
  Py_XDECREF(seq);
  Py_XDECREF(ret1);
  Py_XDECREF(ret2);
  return ok;
}

bool PyUpb_Repeated_Init(PyObject* m) {
  PyUpb_ModuleState* state = PyUpb_ModuleState_GetFromModule(m);

  state->repeated_composite_container_type =
      PyUpb_AddClass(m, &PyUpb_RepeatedCompositeContainer_Spec);
  state->repeated_scalar_container_type =
      PyUpb_AddClass(m, &PyUpb_RepeatedScalarContainer_Spec);

  return state->repeated_composite_container_type &&
         state->repeated_scalar_container_type &&
         PyUpb_Repeated_RegisterAsSequence(state);
}
