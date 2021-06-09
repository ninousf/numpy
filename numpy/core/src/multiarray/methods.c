#define PY_SSIZE_T_CLEAN
#include <stdarg.h>
#include <Python.h>
#include "structmember.h"

#define NPY_NO_DEPRECATED_API NPY_API_VERSION
#define _MULTIARRAYMODULE
#include "numpy/arrayobject.h"
#include "arrayobject.h"
#include "numpy/arrayscalars.h"

#include "arrayfunction_override.h"
#include "npy_argparse.h"
#include "npy_config.h"
#include "npy_pycompat.h"
#include "npy_import.h"
#include "ufunc_override.h"
#include "array_coercion.h"
#include "common.h"
#include "templ_common.h" /* for npy_mul_with_overflow_intp */
#include "ctors.h"
#include "calculation.h"
#include "convert_datatype.h"
#include "item_selection.h"
#include "conversion_utils.h"
#include "shape.h"
#include "strfuncs.h"
#include "array_assign.h"

#include "methods.h"
#include "alloc.h"


/* NpyArg_ParseKeywords
 *
 * Utility function that provides the keyword parsing functionality of
 * PyArg_ParseTupleAndKeywords without having to have an args argument.
 *
 */
static int
NpyArg_ParseKeywords(PyObject *keys, const char *format, char **kwlist, ...)
{
    PyObject *args = PyTuple_New(0);
    int ret;
    va_list va;

    if (args == NULL) {
        PyErr_SetString(PyExc_RuntimeError,
                "Failed to allocate new tuple");
        return 0;
    }
    va_start(va, kwlist);
    ret = PyArg_VaParseTupleAndKeywords(args, keys, format, kwlist, va);
    va_end(va);
    Py_DECREF(args);
    return ret;
}


/*
 * Forwards an ndarray method to a the Python function
 * numpy.core._methods.<name>(...)
 */
static PyObject *
forward_ndarray_method(PyArrayObject *self, PyObject *args, PyObject *kwds,
                            PyObject *forwarding_callable)
{
    PyObject *sargs, *ret;
    int i, n;

    /* Combine 'self' and 'args' together into one tuple */
    n = PyTuple_GET_SIZE(args);
    sargs = PyTuple_New(n + 1);
    if (sargs == NULL) {
        return NULL;
    }
    Py_INCREF(self);
    PyTuple_SET_ITEM(sargs, 0, (PyObject *)self);
    for (i = 0; i < n; ++i) {
        PyObject *item = PyTuple_GET_ITEM(args, i);
        Py_INCREF(item);
        PyTuple_SET_ITEM(sargs, i+1, item);
    }

    /* Call the function and return */
    ret = PyObject_Call(forwarding_callable, sargs, kwds);
    Py_DECREF(sargs);
    return ret;
}

/*
 * Forwards an ndarray method to the function numpy.core._methods.<name>(...),
 * caching the callable in a local static variable. Note that the
 * initialization is not thread-safe, but relies on the CPython GIL to
 * be correct.
 */
#define NPY_FORWARD_NDARRAY_METHOD(name) \
        static PyObject *callable = NULL; \
        npy_cache_import("numpy.core._methods", name, &callable); \
        if (callable == NULL) { \
            return NULL; \
        } \
        return forward_ndarray_method(self, args, kwds, callable)


static PyObject *
array_take(PyArrayObject *self,
        PyObject *const *args, Py_ssize_t len_args, PyObject *kwnames)
{
    int dimension = NPY_MAXDIMS;
    PyObject *indices;
    PyArrayObject *out = NULL;
    NPY_CLIPMODE mode = NPY_RAISE;
    NPY_PREPARE_ARGPARSER;

    if (npy_parse_arguments("take", args, len_args, kwnames,
            "indices", NULL, &indices,
            "|axis", &PyArray_AxisConverter, &dimension,
            "|out", &PyArray_OutputConverter, &out,
            "|mode", &PyArray_ClipmodeConverter, &mode,
            NULL, NULL, NULL) < 0) {
        return NULL;
    }

    PyObject *ret = PyArray_TakeFrom(self, indices, dimension, out, mode);

    /* this matches the unpacking behavior of ufuncs */
    if (out == NULL) {
        return PyArray_Return((PyArrayObject *)ret);
    }
    else {
        return ret;
    }
}

static PyObject *
array_fill(PyArrayObject *self, PyObject *args)
{
    PyObject *obj;
    if (!PyArg_ParseTuple(args, "O:fill", &obj)) {
        return NULL;
    }
    if (PyArray_FillWithScalar(self, obj) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
array_put(PyArrayObject *self, PyObject *args, PyObject *kwds)
{
    PyObject *indices, *values;
    NPY_CLIPMODE mode = NPY_RAISE;
    static char *kwlist[] = {"indices", "values", "mode", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO|O&:put", kwlist,
                                     &indices,
                                     &values,
                                     PyArray_ClipmodeConverter, &mode))
        return NULL;
    return PyArray_PutTo(self, values, indices, mode);
}

static PyObject *
array_reshape(PyArrayObject *self, PyObject *args, PyObject *kwds)
{
    static char *keywords[] = {"order", NULL};
    PyArray_Dims newshape;
    PyObject *ret;
    NPY_ORDER order = NPY_CORDER;
    Py_ssize_t n = PyTuple_Size(args);

    if (!NpyArg_ParseKeywords(kwds, "|O&", keywords,
                PyArray_OrderConverter, &order)) {
        return NULL;
    }

    if (n <= 1) {
        if (n != 0 && PyTuple_GET_ITEM(args, 0) == Py_None) {
            return PyArray_View(self, NULL, NULL);
        }
        if (!PyArg_ParseTuple(args, "O&:reshape", PyArray_IntpConverter,
                              &newshape)) {
            return NULL;
        }
    }
    else {
        if (!PyArray_IntpConverter(args, &newshape)) {
            if (!PyErr_Occurred()) {
                PyErr_SetString(PyExc_TypeError,
                                "invalid shape");
            }
            goto fail;
        }
    }
    ret = PyArray_Newshape(self, &newshape, order);
    npy_free_cache_dim_obj(newshape);
    return ret;

 fail:
    npy_free_cache_dim_obj(newshape);
    return NULL;
}

static PyObject *
array_squeeze(PyArrayObject *self,
        PyObject *const *args, Py_ssize_t len_args, PyObject *kwnames)
{
    PyObject *axis_in = NULL;
    npy_bool axis_flags[NPY_MAXDIMS];
    NPY_PREPARE_ARGPARSER;

    if (npy_parse_arguments("squeeze", args, len_args, kwnames,
            "|axis", NULL, &axis_in,
            NULL, NULL, NULL) < 0) {
        return NULL;
    }

    if (axis_in == NULL || axis_in == Py_None) {
        return PyArray_Squeeze(self);
    }
    else {
        if (PyArray_ConvertMultiAxis(axis_in, PyArray_NDIM(self),
                                            axis_flags) != NPY_SUCCEED) {
            return NULL;
        }

        return PyArray_SqueezeSelected(self, axis_flags);
    }
}

static PyObject *
array_view(PyArrayObject *self,
        PyObject *const *args, Py_ssize_t len_args, PyObject *kwnames)
{
    PyObject *out_dtype = NULL;
    PyObject *out_type = NULL;
    PyArray_Descr *dtype = NULL;
    NPY_PREPARE_ARGPARSER;

    if (npy_parse_arguments("view", args, len_args, kwnames,
            "|dtype", NULL, &out_dtype,
            "|type", NULL, &out_type,
            NULL, NULL, NULL) < 0) {
        return NULL;
    }

    /* If user specified a positional argument, guess whether it
       represents a type or a dtype for backward compatibility. */
    if (out_dtype) {
        /* type specified? */
        if (PyType_Check(out_dtype) &&
            PyType_IsSubtype((PyTypeObject *)out_dtype,
                             &PyArray_Type)) {
            if (out_type) {
                PyErr_SetString(PyExc_ValueError,
                                "Cannot specify output type twice.");
                return NULL;
            }
            out_type = out_dtype;
            out_dtype = NULL;
        }
    }

    if ((out_type) && (!PyType_Check(out_type) ||
                       !PyType_IsSubtype((PyTypeObject *)out_type,
                                         &PyArray_Type))) {
        PyErr_SetString(PyExc_ValueError,
                        "Type must be a sub-type of ndarray type");
        return NULL;
    }

    if ((out_dtype) &&
        (PyArray_DescrConverter(out_dtype, &dtype) == NPY_FAIL)) {
        return NULL;
    }

    return PyArray_View(self, dtype, (PyTypeObject*)out_type);
}

static PyObject *
array_argmax(PyArrayObject *self,
        PyObject *const *args, Py_ssize_t len_args, PyObject *kwnames)
{
    int axis = NPY_MAXDIMS;
    PyArrayObject *out = NULL;
    NPY_PREPARE_ARGPARSER;

    if (npy_parse_arguments("argmax", args, len_args, kwnames,
            "|axis", &PyArray_AxisConverter, &axis,
            "|out", &PyArray_OutputConverter, &out,
            NULL, NULL, NULL) < 0) {
        return NULL;
    }

    PyObject *ret = PyArray_ArgMax(self, axis, out);

    /* this matches the unpacking behavior of ufuncs */
    if (out == NULL) {
        return PyArray_Return((PyArrayObject *)ret);
    }
    else {
        return ret;
    }
}

static PyObject *
array_argmin(PyArrayObject *self,
        PyObject *const *args, Py_ssize_t len_args, PyObject *kwnames)
{
    int axis = NPY_MAXDIMS;
    PyArrayObject *out = NULL;
    NPY_PREPARE_ARGPARSER;

    if (npy_parse_arguments("argmin", args, len_args, kwnames,
            "|axis", &PyArray_AxisConverter, &axis,
            "|out", &PyArray_OutputConverter, &out,
            NULL, NULL, NULL) < 0) {
        return NULL;
    }

    PyObject *ret = PyArray_ArgMin(self, axis, out);

    /* this matches the unpacking behavior of ufuncs */
    if (out == NULL) {
        return PyArray_Return((PyArrayObject *)ret);
    }
    else {
        return ret;
    }
}

static PyObject *
array_max(PyArrayObject *self, PyObject *args, PyObject *kwds)
{
    NPY_FORWARD_NDARRAY_METHOD("_amax");
}

static PyObject *
array_min(PyArrayObject *self, PyObject *args, PyObject *kwds)
{
    NPY_FORWARD_NDARRAY_METHOD("_amin");
}

static PyObject *
array_ptp(PyArrayObject *self, PyObject *args, PyObject *kwds)
{
    NPY_FORWARD_NDARRAY_METHOD("_ptp");
}


static PyObject *
array_swapaxes(PyArrayObject *self, PyObject *args)
{
    int axis1, axis2;

    if (!PyArg_ParseTuple(args, "ii:swapaxes", &axis1, &axis2)) {
        return NULL;
    }
    return PyArray_SwapAxes(self, axis1, axis2);
}


/*NUMPY_API
  Get a subset of bytes from each element of the array
  steals reference to typed, must not be NULL
*/
NPY_NO_EXPORT PyObject *
PyArray_GetField(PyArrayObject *self, PyArray_Descr *typed, int offset)
{
    PyObject *ret = NULL;
    PyObject *safe;
    static PyObject *checkfunc = NULL;
    int self_elsize, typed_elsize;

    /* check that we are not reinterpreting memory containing Objects. */
    if (_may_have_objects(PyArray_DESCR(self)) || _may_have_objects(typed)) {
        npy_cache_import("numpy.core._internal", "_getfield_is_safe",
                         &checkfunc);
        if (checkfunc == NULL) {
            Py_DECREF(typed);
            return NULL;
        }

        /* only returns True or raises */
        safe = PyObject_CallFunction(checkfunc, "OOi", PyArray_DESCR(self),
                                     typed, offset);
        if (safe == NULL) {
            Py_DECREF(typed);
            return NULL;
        }
        Py_DECREF(safe);
    }
    self_elsize = PyArray_ITEMSIZE(self);
    typed_elsize = typed->elsize;

    /* check that values are valid */
    if (typed_elsize > self_elsize) {
        PyErr_SetString(PyExc_ValueError, "new type is larger than original type");
        Py_DECREF(typed);
        return NULL;
    }
    if (offset < 0) {
        PyErr_SetString(PyExc_ValueError, "offset is negative");
        Py_DECREF(typed);
        return NULL;
    }
    if (offset > self_elsize - typed_elsize) {
        PyErr_SetString(PyExc_ValueError, "new type plus offset is larger than original type");
        Py_DECREF(typed);
        return NULL;
    }

    ret = PyArray_NewFromDescr_int(
            Py_TYPE(self), typed,
            PyArray_NDIM(self), PyArray_DIMS(self), PyArray_STRIDES(self),
            PyArray_BYTES(self) + offset,
            PyArray_FLAGS(self) & ~NPY_ARRAY_F_CONTIGUOUS,
            (PyObject *)self, (PyObject *)self,
            0, 1);
    return ret;
}

static PyObject *
array_getfield(PyArrayObject *self, PyObject *args, PyObject *kwds)
{

    PyArray_Descr *dtype = NULL;
    int offset = 0;
    static char *kwlist[] = {"dtype", "offset", 0};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O&|i:getfield", kwlist,
                                     PyArray_DescrConverter, &dtype,
                                     &offset)) {
        Py_XDECREF(dtype);
        return NULL;
    }

    return PyArray_GetField(self, dtype, offset);
}


/*NUMPY_API
  Set a subset of bytes from each element of the array
  steals reference to dtype, must not be NULL
*/
NPY_NO_EXPORT int
PyArray_SetField(PyArrayObject *self, PyArray_Descr *dtype,
                 int offset, PyObject *val)
{
    PyObject *ret = NULL;
    int retval = 0;

    if (PyArray_FailUnlessWriteable(self, "assignment destination") < 0) {
        Py_DECREF(dtype);
        return -1;
    }

    /* getfield returns a view we can write to */
    ret = PyArray_GetField(self, dtype, offset);
    if (ret == NULL) {
        return -1;
    }

    retval = PyArray_CopyObject((PyArrayObject *)ret, val);
    Py_DECREF(ret);
    return retval;
}

static PyObject *
array_setfield(PyArrayObject *self, PyObject *args, PyObject *kwds)
{
    PyArray_Descr *dtype = NULL;
    int offset = 0;
    PyObject *value;
    static char *kwlist[] = {"value", "dtype", "offset", 0};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OO&|i:setfield", kwlist,
                                     &value,
                                     PyArray_DescrConverter, &dtype,
                                     &offset)) {
        Py_XDECREF(dtype);
        return NULL;
    }

    if (PyArray_SetField(self, dtype, offset, value) < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

/* This doesn't change the descriptor just the actual data...
 */

/*NUMPY_API*/
NPY_NO_EXPORT PyObject *
PyArray_Byteswap(PyArrayObject *self, npy_bool inplace)
{
    PyArrayObject *ret;
    npy_intp size;
    PyArray_CopySwapNFunc *copyswapn;
    PyArrayIterObject *it;

    copyswapn = PyArray_DESCR(self)->f->copyswapn;
    if (inplace) {
        if (PyArray_FailUnlessWriteable(self, "array to be byte-swapped") < 0) {
            return NULL;
        }
        size = PyArray_SIZE(self);
        if (PyArray_ISONESEGMENT(self)) {
            copyswapn(PyArray_DATA(self), PyArray_DESCR(self)->elsize, NULL, -1, size, 1, self);
        }
        else { /* Use iterator */
            int axis = -1;
            npy_intp stride;
            it = (PyArrayIterObject *)                      \
                PyArray_IterAllButAxis((PyObject *)self, &axis);
            stride = PyArray_STRIDES(self)[axis];
            size = PyArray_DIMS(self)[axis];
            while (it->index < it->size) {
                copyswapn(it->dataptr, stride, NULL, -1, size, 1, self);
                PyArray_ITER_NEXT(it);
            }
            Py_DECREF(it);
        }

        Py_INCREF(self);
        return (PyObject *)self;
    }
    else {
        PyObject *new;
        if ((ret = (PyArrayObject *)PyArray_NewCopy(self,-1)) == NULL) {
            return NULL;
        }
        new = PyArray_Byteswap(ret, NPY_TRUE);
        Py_DECREF(new);
        return (PyObject *)ret;
    }
}


static PyObject *
array_byteswap(PyArrayObject *self, PyObject *args, PyObject *kwds)
{
    npy_bool inplace = NPY_FALSE;
    static char *kwlist[] = {"inplace", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O&:byteswap", kwlist,
                                     PyArray_BoolConverter, &inplace)) {
        return NULL;
    }
    return PyArray_Byteswap(self, inplace);
}

static PyObject *
array_tolist(PyArrayObject *self, PyObject *args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return NULL;
    }
    return PyArray_ToList(self);
}


static PyObject *
array_tobytes(PyArrayObject *self, PyObject *args, PyObject *kwds)
{
    NPY_ORDER order = NPY_CORDER;
    static char *kwlist[] = {"order", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O&:tobytes", kwlist,
                                     PyArray_OrderConverter, &order)) {
        return NULL;
    }
    return PyArray_ToString(self, order);
}

static PyObject *
array_tostring(PyArrayObject *self, PyObject *args, PyObject *kwds)
{
    NPY_ORDER order = NPY_CORDER;
    static char *kwlist[] = {"order", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O&:tostring", kwlist,
                                     PyArray_OrderConverter, &order)) {
        return NULL;
    }
    /* 2020-03-30, NumPy 1.19 */
    if (DEPRECATE("tostring() is deprecated. Use tobytes() instead.") < 0) {
        return NULL;
    }
    return PyArray_ToString(self, order);
}

/* Like PyArray_ToFile but takes the file as a python object */
static int
PyArray_ToFileObject(PyArrayObject *self, PyObject *file, char *sep, char *format)
{
    npy_off_t orig_pos = 0;
    FILE *fd = npy_PyFile_Dup2(file, "wb", &orig_pos);

    if (fd == NULL) {
        return -1;
    }

    int write_ret = PyArray_ToFile(self, fd, sep, format);
    PyObject *err_type, *err_value, *err_traceback;
    PyErr_Fetch(&err_type, &err_value, &err_traceback);
    int close_ret = npy_PyFile_DupClose2(file, fd, orig_pos);
    npy_PyErr_ChainExceptions(err_type, err_value, err_traceback);

    if (write_ret || close_ret) {
        return -1;
    }
    return 0;
}

/* This should grow an order= keyword to be consistent
 */

static PyObject *
array_tofile(PyArrayObject *self, PyObject *args, PyObject *kwds)
{
    int own;
    PyObject *file;
    char *sep = "";
    char *format = "";
    static char *kwlist[] = {"file", "sep", "format", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|ss:tofile", kwlist,
                                     &file,
                                     &sep,
                                     &format)) {
        return NULL;
    }

    file = NpyPath_PathlikeToFspath(file);
    if (file == NULL) {
        return NULL;
    }
    if (PyBytes_Check(file) || PyUnicode_Check(file)) {
        Py_SETREF(file, npy_PyFile_OpenFile(file, "wb"));
        if (file == NULL) {
            return NULL;
        }
        own = 1;
    }
    else {
        own = 0;
    }

    int file_ret = PyArray_ToFileObject(self, file, sep, format);
    int close_ret = 0;

    if (own) {
        PyObject *err_type, *err_value, *err_traceback;
        PyErr_Fetch(&err_type, &err_value, &err_traceback);
        close_ret = npy_PyFile_CloseFile(file);
        npy_PyErr_ChainExceptions(err_type, err_value, err_traceback);
    }

    Py_DECREF(file);

    if (file_ret || close_ret) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
array_toscalar(PyArrayObject *self, PyObject *args)
{
    npy_intp multi_index[NPY_MAXDIMS];
    int n = PyTuple_GET_SIZE(args);
    int idim, ndim = PyArray_NDIM(self);

    /* If there is a tuple as a single argument, treat it as the argument */
    if (n == 1 && PyTuple_Check(PyTuple_GET_ITEM(args, 0))) {
        args = PyTuple_GET_ITEM(args, 0);
        n = PyTuple_GET_SIZE(args);
    }

    if (n == 0) {
        if (PyArray_SIZE(self) == 1) {
            for (idim = 0; idim < ndim; ++idim) {
                multi_index[idim] = 0;
            }
        }
        else {
            PyErr_SetString(PyExc_ValueError,
                    "can only convert an array of size 1 to a Python scalar");
            return NULL;
        }
    }
    /* Special case of C-order flat indexing... :| */
    else if (n == 1 && ndim != 1) {
        npy_intp *shape = PyArray_SHAPE(self);
        npy_intp value, size = PyArray_SIZE(self);

        value = PyArray_PyIntAsIntp(PyTuple_GET_ITEM(args, 0));
        if (error_converting(value)) {
            return NULL;
        }

        if (check_and_adjust_index(&value, size, -1, NULL) < 0) {
            return NULL;
        }

        /* Convert the flat index into a multi-index */
        for (idim = ndim-1; idim >= 0; --idim) {
            multi_index[idim] = value % shape[idim];
            value /= shape[idim];
        }
    }
    /* A multi-index tuple */
    else if (n == ndim) {
        npy_intp value;

        for (idim = 0; idim < ndim; ++idim) {
            value = PyArray_PyIntAsIntp(PyTuple_GET_ITEM(args, idim));
            if (error_converting(value)) {
                return NULL;
            }
            multi_index[idim] = value;
        }
    }
    else {
        PyErr_SetString(PyExc_ValueError,
                "incorrect number of indices for array");
        return NULL;
    }

    return PyArray_MultiIndexGetItem(self, multi_index);
}

static PyObject *
array_setscalar(PyArrayObject *self, PyObject *args)
{
    npy_intp multi_index[NPY_MAXDIMS];
    int n = PyTuple_GET_SIZE(args) - 1;
    int idim, ndim = PyArray_NDIM(self);
    PyObject *obj;

    if (n < 0) {
        PyErr_SetString(PyExc_ValueError,
                "itemset must have at least one argument");
        return NULL;
    }
    if (PyArray_FailUnlessWriteable(self, "assignment destination") < 0) {
        return NULL;
    }

    obj = PyTuple_GET_ITEM(args, n);

    /* If there is a tuple as a single argument, treat it as the argument */
    if (n == 1 && PyTuple_Check(PyTuple_GET_ITEM(args, 0))) {
        args = PyTuple_GET_ITEM(args, 0);
        n = PyTuple_GET_SIZE(args);
    }

    if (n == 0) {
        if (PyArray_SIZE(self) == 1) {
            for (idim = 0; idim < ndim; ++idim) {
                multi_index[idim] = 0;
            }
        }
        else {
            PyErr_SetString(PyExc_ValueError,
                    "can only convert an array of size 1 to a Python scalar");
            return NULL;
        }
    }
    /* Special case of C-order flat indexing... :| */
    else if (n == 1 && ndim != 1) {
        npy_intp *shape = PyArray_SHAPE(self);
        npy_intp value, size = PyArray_SIZE(self);

        value = PyArray_PyIntAsIntp(PyTuple_GET_ITEM(args, 0));
        if (error_converting(value)) {
            return NULL;
        }

        if (check_and_adjust_index(&value, size, -1, NULL) < 0) {
            return NULL;
        }

        /* Convert the flat index into a multi-index */
        for (idim = ndim-1; idim >= 0; --idim) {
            multi_index[idim] = value % shape[idim];
            value /= shape[idim];
        }
    }
    /* A multi-index tuple */
    else if (n == ndim) {
        npy_intp value;

        for (idim = 0; idim < ndim; ++idim) {
            value = PyArray_PyIntAsIntp(PyTuple_GET_ITEM(args, idim));
            if (error_converting(value)) {
                return NULL;
            }
            multi_index[idim] = value;
        }
    }
    else {
        PyErr_SetString(PyExc_ValueError,
                "incorrect number of indices for array");
        return NULL;
    }

    if (PyArray_MultiIndexSetItem(self, multi_index, obj) < 0) {
        return NULL;
    }
    else {
        Py_RETURN_NONE;
    }
}


static PyObject *
array_astype(PyArrayObject *self,
        PyObject *const *args, Py_ssize_t len_args, PyObject *kwnames)
{
    PyArray_Descr *dtype = NULL;
    /*
     * TODO: UNSAFE default for compatibility, I think
     *       switching to SAME_KIND by default would be good.
     */
    NPY_CASTING casting = NPY_UNSAFE_CASTING;
    NPY_ORDER order = NPY_KEEPORDER;
    PyArray_CopyMode forcecopy = 1;
    int subok = 1;
    NPY_PREPARE_ARGPARSER;
    if (npy_parse_arguments("astype", args, len_args, kwnames,
            "dtype", &PyArray_DescrConverter, &dtype,
            "|order", &PyArray_OrderConverter, &order,
            "|casting", &PyArray_CastingConverter, &casting,
            "|subok", &PyArray_PythonPyIntFromInt, &subok,
            "|copy", &PyArray_CopyConverter, &forcecopy,
            NULL, NULL, NULL) < 0) {
        Py_XDECREF(dtype);
        return NULL;
    }

    /* If it is not a concrete dtype instance find the best one for the array */
    Py_SETREF(dtype, PyArray_AdaptDescriptorToArray(self, (PyObject *)dtype));
    if (dtype == NULL) {
        return NULL;
    }

    /*
     * If the memory layout matches and, data types are equivalent,
     * and it's not a subtype if subok is False, then we
     * can skip the copy.
     */
    if (forcecopy != NPY_COPY_ALWAYS && 
        (order == NPY_KEEPORDER ||
        (order == NPY_ANYORDER &&
            (PyArray_IS_C_CONTIGUOUS(self) ||
            PyArray_IS_F_CONTIGUOUS(self))) ||
        (order == NPY_CORDER &&
            PyArray_IS_C_CONTIGUOUS(self)) ||
        (order == NPY_FORTRANORDER &&
            PyArray_IS_F_CONTIGUOUS(self))) &&
    (subok || PyArray_CheckExact(self)) &&
    PyArray_EquivTypes(dtype, PyArray_DESCR(self))) {
        Py_DECREF(dtype);
        Py_INCREF(self);
        return (PyObject *)self;
    }

    if( forcecopy == NPY_COPY_NEVER ) {
        PyErr_SetString(PyExc_ValueError,
                        "Unable to avoid copy while casting in np.CopyMode.NEVER");
        Py_DECREF(dtype);
        return NULL;
    }
    
    if (!PyArray_CanCastArrayTo(self, dtype, casting)) {
        PyErr_Clear();
        npy_set_invalid_cast_error(
                PyArray_DESCR(self), dtype, casting, PyArray_NDIM(self) == 0);
        Py_DECREF(dtype);
        return NULL;
    }

    PyArrayObject *ret;

    /* This steals the reference to dtype, so no DECREF of dtype */
    ret = (PyArrayObject *)PyArray_NewLikeArray(
                                self, order, dtype, subok);
    if (ret == NULL) {
        return NULL;
    }
    /* NumPy 1.20, 2020-10-01 */
    if ((PyArray_NDIM(self) != PyArray_NDIM(ret)) &&
            DEPRECATE_FUTUREWARNING(
                "casting an array to a subarray dtype "
                "will not use broadcasting in the future, but cast each "
                "element to the new dtype and then append the dtype's shape "
                "to the new array. You can opt-in to the new behaviour, by "
                "additional field to the cast: "
                "`arr.astype(np.dtype([('f', dtype)]))['f']`.\n"
                "This may lead to a different result or to current failures "
                "succeeding.  "
                "(FutureWarning since NumPy 1.20)") < 0) {
        Py_DECREF(ret);
        return NULL;
    }

    if (PyArray_CopyInto(ret, self) < 0) {
        Py_DECREF(ret);
        return NULL;
    }

    return (PyObject *)ret;
}

/* default sub-type implementation */


static PyObject *
array_wraparray(PyArrayObject *self, PyObject *args)
{
    PyArrayObject *arr;
    PyObject *obj;

    if (PyTuple_Size(args) < 1) {
        PyErr_SetString(PyExc_TypeError,
                        "only accepts 1 argument");
        return NULL;
    }
    obj = PyTuple_GET_ITEM(args, 0);
    if (obj == NULL) {
        return NULL;
    }
    if (!PyArray_Check(obj)) {
        PyErr_SetString(PyExc_TypeError,
                        "can only be called with ndarray object");
        return NULL;
    }
    arr = (PyArrayObject *)obj;

    if (Py_TYPE(self) != Py_TYPE(arr)) {
        PyArray_Descr *dtype = PyArray_DESCR(arr);
        Py_INCREF(dtype);
        return PyArray_NewFromDescrAndBase(
                Py_TYPE(self),
                dtype,
                PyArray_NDIM(arr),
                PyArray_DIMS(arr),
                PyArray_STRIDES(arr), PyArray_DATA(arr),
                PyArray_FLAGS(arr), (PyObject *)self, obj);
    } else {
        /*The type was set in __array_prepare__*/
        Py_INCREF(arr);
        return (PyObject *)arr;
    }
}


static PyObject *
array_preparearray(PyArrayObject *self, PyObject *args)
{
    PyObject *obj;
    PyArrayObject *arr;
    PyArray_Descr *dtype;

    if (PyTuple_Size(args) < 1) {
        PyErr_SetString(PyExc_TypeError,
                        "only accepts 1 argument");
        return NULL;
    }
    obj = PyTuple_GET_ITEM(args, 0);
    if (!PyArray_Check(obj)) {
        PyErr_SetString(PyExc_TypeError,
                        "can only be called with ndarray object");
        return NULL;
    }
    arr = (PyArrayObject *)obj;

    if (Py_TYPE(self) == Py_TYPE(arr)) {
        /* No need to create a new view */
        Py_INCREF(arr);
        return (PyObject *)arr;
    }

    dtype = PyArray_DESCR(arr);
    Py_INCREF(dtype);
    return PyArray_NewFromDescrAndBase(
            Py_TYPE(self), dtype,
            PyArray_NDIM(arr), PyArray_DIMS(arr), PyArray_STRIDES(arr),
            PyArray_DATA(arr),
            PyArray_FLAGS(arr), (PyObject *)self, (PyObject *)arr);
}


static PyObject *
array_getarray(PyArrayObject *self, PyObject *args)
{
    PyArray_Descr *newtype = NULL;
    PyObject *ret;

    if (!PyArg_ParseTuple(args, "|O&:__array__",
                            PyArray_DescrConverter, &newtype)) {
        Py_XDECREF(newtype);
        return NULL;
    }

    /* convert to PyArray_Type */
    if (!PyArray_CheckExact(self)) {
        PyArrayObject *new;

        Py_INCREF(PyArray_DESCR(self));
        new = (PyArrayObject *)PyArray_NewFromDescrAndBase(
                &PyArray_Type,
                PyArray_DESCR(self),
                PyArray_NDIM(self),
                PyArray_DIMS(self),
                PyArray_STRIDES(self),
                PyArray_DATA(self),
                PyArray_FLAGS(self),
                NULL,
                (PyObject *)self
        );
        if (new == NULL) {
            return NULL;
        }
        self = new;
    }
    else {
        Py_INCREF(self);
    }

    if ((newtype == NULL) || PyArray_EquivTypes(PyArray_DESCR(self), newtype)) {
        return (PyObject *)self;
    }
    else {
        ret = PyArray_CastToType(self, newtype, 0);
        Py_DECREF(self);
        return ret;
    }
}

/*
 * Check whether any of the input and output args have a non-default
 * __array_ufunc__ method. Return 1 if so, 0 if not, and -1 on error.
 *
 * This function primarily exists to help ndarray.__array_ufunc__ determine
 * whether it can support a ufunc (which is the case only if none of the
 * operands have an override).  Thus, unlike in umath/override.c, the
 * actual overrides are not needed and one can stop looking once one is found.
 */
static int
any_array_ufunc_overrides(PyObject *args, PyObject *kwds)
{
    int i;
    int nin, nout;
    PyObject *out_kwd_obj;
    PyObject *fast;
    PyObject **in_objs, **out_objs;

    /* check inputs */
    nin = PyTuple_Size(args);
    if (nin < 0) {
        return -1;
    }
    fast = PySequence_Fast(args, "Could not convert object to sequence");
    if (fast == NULL) {
        return -1;
    }
    in_objs = PySequence_Fast_ITEMS(fast);
    for (i = 0; i < nin; ++i) {
        if (PyUFunc_HasOverride(in_objs[i])) {
            Py_DECREF(fast);
            return 1;
        }
    }
    Py_DECREF(fast);
    /* check outputs, if any */
    nout = PyUFuncOverride_GetOutObjects(kwds, &out_kwd_obj, &out_objs);
    if (nout < 0) {
        return -1;
    }
    for (i = 0; i < nout; i++) {
        if (PyUFunc_HasOverride(out_objs[i])) {
            Py_DECREF(out_kwd_obj);
            return 1;
        }
    }
    Py_DECREF(out_kwd_obj);
    return 0;
}


NPY_NO_EXPORT PyObject *
array_ufunc(PyArrayObject *NPY_UNUSED(self), PyObject *args, PyObject *kwds)
{
    PyObject *ufunc, *method_name, *normal_args, *ufunc_method;
    PyObject *result = NULL;
    int has_override;

    assert(PyTuple_CheckExact(args));
    assert(kwds == NULL || PyDict_CheckExact(kwds));

    if (PyTuple_GET_SIZE(args) < 2) {
        PyErr_SetString(PyExc_TypeError,
                        "__array_ufunc__ requires at least 2 arguments");
        return NULL;
    }
    normal_args = PyTuple_GetSlice(args, 2, PyTuple_GET_SIZE(args));
    if (normal_args == NULL) {
        return NULL;
    }
    /* ndarray cannot handle overrides itself */
    has_override = any_array_ufunc_overrides(normal_args, kwds);
    if (has_override < 0) {
        goto cleanup;
    }
    else if (has_override) {
        result = Py_NotImplemented;
        Py_INCREF(Py_NotImplemented);
        goto cleanup;
    }

    ufunc = PyTuple_GET_ITEM(args, 0);
    method_name = PyTuple_GET_ITEM(args, 1);
    /*
     * TODO(?): call into UFunc code at a later point, since here arguments are
     * already normalized and we do not have to look for __array_ufunc__ again.
     */
    ufunc_method = PyObject_GetAttr(ufunc, method_name);
    if (ufunc_method == NULL) {
        goto cleanup;
    }
    result = PyObject_Call(ufunc_method, normal_args, kwds);
    Py_DECREF(ufunc_method);

cleanup:
    Py_DECREF(normal_args);
    /* no need to DECREF borrowed references ufunc and method_name */
    return result;
}

static PyObject *
array_function(PyArrayObject *NPY_UNUSED(self), PyObject *c_args, PyObject *c_kwds)
{
    PyObject *func, *types, *args, *kwargs, *result;
    static char *kwlist[] = {"func", "types", "args", "kwargs", NULL};

    if (!PyArg_ParseTupleAndKeywords(
            c_args, c_kwds, "OOOO:__array_function__", kwlist,
            &func, &types, &args, &kwargs)) {
        return NULL;
    }

    types = PySequence_Fast(
        types,
        "types argument to ndarray.__array_function__ must be iterable");
    if (types == NULL) {
        return NULL;
    }

    result = array_function_method_impl(func, types, args, kwargs);
    Py_DECREF(types);
    return result;
}

static PyObject *
array_copy(PyArrayObject *self,
        PyObject *const *args, Py_ssize_t len_args, PyObject *kwnames)
{
    NPY_ORDER order = NPY_CORDER;
    NPY_PREPARE_ARGPARSER;

    if (npy_parse_arguments("copy", args, len_args, kwnames,
            "|order", PyArray_OrderConverter, &order,
            NULL, NULL, NULL) < 0) {
        return NULL;
    }

    return PyArray_NewCopy(self, order);
}

/* Separate from array_copy to make __copy__ preserve Fortran contiguity. */
static PyObject *
array_copy_keeporder(PyArrayObject *self, PyObject *args)
{
    if (!PyArg_ParseTuple(args, ":__copy__")) {
        return NULL;
    }
    return PyArray_NewCopy(self, NPY_KEEPORDER);
}

#include <stdio.h>
static PyObject *
array_resize(PyArrayObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"refcheck", NULL};
    Py_ssize_t size = PyTuple_Size(args);
    int refcheck = 1;
    PyArray_Dims newshape;
    PyObject *ret, *obj;


    if (!NpyArg_ParseKeywords(kwds, "|i", kwlist,  &refcheck)) {
        return NULL;
    }

    if (size == 0) {
        Py_RETURN_NONE;
    }
    else if (size == 1) {
        obj = PyTuple_GET_ITEM(args, 0);
        if (obj == Py_None) {
            Py_RETURN_NONE;
        }
        args = obj;
    }
    if (!PyArray_IntpConverter(args, &newshape)) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_TypeError, "invalid shape");
        }
        return NULL;
    }

    ret = PyArray_Resize(self, &newshape, refcheck, NPY_ANYORDER);
    npy_free_cache_dim_obj(newshape);
    if (ret == NULL) {
        return NULL;
    }
    Py_DECREF(ret);
    Py_RETURN_NONE;
}

static PyObject *
array_repeat(PyArrayObject *self, PyObject *args, PyObject *kwds) {
    PyObject *repeats;
    int axis = NPY_MAXDIMS;
    static char *kwlist[] = {"repeats", "axis", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O&:repeat", kwlist,
                                     &repeats,
                                     PyArray_AxisConverter, &axis)) {
        return NULL;
    }
    return PyArray_Return((PyArrayObject *)PyArray_Repeat(self, repeats, axis));
}

static PyObject *
array_choose(PyArrayObject *self, PyObject *args, PyObject *kwds)
{
    static char *keywords[] = {"out", "mode", NULL};
    PyObject *choices;
    PyArrayObject *out = NULL;
    NPY_CLIPMODE clipmode = NPY_RAISE;
    Py_ssize_t n = PyTuple_Size(args);

    if (n <= 1) {
        if (!PyArg_ParseTuple(args, "O:choose", &choices)) {
            return NULL;
        }
    }
    else {
        choices = args;
    }

    if (!NpyArg_ParseKeywords(kwds, "|O&O&", keywords,
                PyArray_OutputConverter, &out,
                PyArray_ClipmodeConverter, &clipmode)) {
        return NULL;
    }

    PyObject *ret = PyArray_Choose(self, choices, out, clipmode);

    /* this matches the unpacking behavior of ufuncs */
    if (out == NULL) {
        return PyArray_Return((PyArrayObject *)ret);
    }
    else {
        return ret;
    }
}

static PyObject *
array_sort(PyArrayObject *self,
        PyObject *const *args, Py_ssize_t len_args, PyObject *kwnames)
{
    int axis=-1;
    int val;
    NPY_SORTKIND sortkind = NPY_QUICKSORT;
    PyObject *order = NULL;
    PyArray_Descr *saved = NULL;
    PyArray_Descr *newd;
    NPY_PREPARE_ARGPARSER;

    if (npy_parse_arguments("sort", args, len_args, kwnames,
            "|axis", &PyArray_PythonPyIntFromInt, &axis,
            "|kind", &PyArray_SortkindConverter, &sortkind,
            "|order", NULL, &order,
            NULL, NULL, NULL) < 0) {
        return NULL;
    }
    if (order == Py_None) {
        order = NULL;
    }
    if (order != NULL) {
        PyObject *new_name;
        PyObject *_numpy_internal;
        saved = PyArray_DESCR(self);
        if (!PyDataType_HASFIELDS(saved)) {
            PyErr_SetString(PyExc_ValueError, "Cannot specify " \
                            "order when the array has no fields.");
            return NULL;
        }
        _numpy_internal = PyImport_ImportModule("numpy.core._internal");
        if (_numpy_internal == NULL) {
            return NULL;
        }
        new_name = PyObject_CallMethod(_numpy_internal, "_newnames",
                                       "OO", saved, order);
        Py_DECREF(_numpy_internal);
        if (new_name == NULL) {
            return NULL;
        }
        newd = PyArray_DescrNew(saved);
        Py_DECREF(newd->names);
        newd->names = new_name;
        ((PyArrayObject_fields *)self)->descr = newd;
    }

    val = PyArray_Sort(self, axis, sortkind);
    if (order != NULL) {
        Py_XDECREF(PyArray_DESCR(self));
        ((PyArrayObject_fields *)self)->descr = saved;
    }
    if (val < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
array_partition(PyArrayObject *self,
        PyObject *const *args, Py_ssize_t len_args, PyObject *kwnames)
{
    int axis=-1;
    int val;
    NPY_SELECTKIND sortkind = NPY_INTROSELECT;
    PyObject *order = NULL;
    PyArray_Descr *saved = NULL;
    PyArray_Descr *newd;
    PyArrayObject * ktharray;
    PyObject * kthobj;
    NPY_PREPARE_ARGPARSER;

    if (npy_parse_arguments("partition", args, len_args, kwnames,
            "kth", NULL, &kthobj,
            "|axis", &PyArray_PythonPyIntFromInt, &axis,
            "|kind", &PyArray_SelectkindConverter, &sortkind,
            "|order", NULL, &order,
            NULL, NULL, NULL) < 0) {
        return NULL;
    }

    if (order == Py_None) {
        order = NULL;
    }
    if (order != NULL) {
        PyObject *new_name;
        PyObject *_numpy_internal;
        saved = PyArray_DESCR(self);
        if (!PyDataType_HASFIELDS(saved)) {
            PyErr_SetString(PyExc_ValueError, "Cannot specify " \
                            "order when the array has no fields.");
            return NULL;
        }
        _numpy_internal = PyImport_ImportModule("numpy.core._internal");
        if (_numpy_internal == NULL) {
            return NULL;
        }
        new_name = PyObject_CallMethod(_numpy_internal, "_newnames",
                                       "OO", saved, order);
        Py_DECREF(_numpy_internal);
        if (new_name == NULL) {
            return NULL;
        }
        newd = PyArray_DescrNew(saved);
        Py_DECREF(newd->names);
        newd->names = new_name;
        ((PyArrayObject_fields *)self)->descr = newd;
    }

    ktharray = (PyArrayObject *)PyArray_FromAny(kthobj, NULL, 0, 1,
                                                NPY_ARRAY_DEFAULT, NULL);
    if (ktharray == NULL)
        return NULL;

    val = PyArray_Partition(self, ktharray, axis, sortkind);
    Py_DECREF(ktharray);

    if (order != NULL) {
        Py_XDECREF(PyArray_DESCR(self));
        ((PyArrayObject_fields *)self)->descr = saved;
    }
    if (val < 0) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
array_argsort(PyArrayObject *self,
        PyObject *const *args, Py_ssize_t len_args, PyObject *kwnames)
{
    int axis = -1;
    NPY_SORTKIND sortkind = NPY_QUICKSORT;
    PyObject *order = NULL, *res;
    PyArray_Descr *newd, *saved=NULL;
    NPY_PREPARE_ARGPARSER;

    if (npy_parse_arguments("argsort", args, len_args, kwnames,
            "|axis", &PyArray_AxisConverter, &axis,
            "|kind", &PyArray_SortkindConverter, &sortkind,
            "|order", NULL, &order,
            NULL, NULL, NULL) < 0) {
        return NULL;
    }
    if (order == Py_None) {
        order = NULL;
    }
    if (order != NULL) {
        PyObject *new_name;
        PyObject *_numpy_internal;
        saved = PyArray_DESCR(self);
        if (!PyDataType_HASFIELDS(saved)) {
            PyErr_SetString(PyExc_ValueError, "Cannot specify "
                            "order when the array has no fields.");
            return NULL;
        }
        _numpy_internal = PyImport_ImportModule("numpy.core._internal");
        if (_numpy_internal == NULL) {
            return NULL;
        }
        new_name = PyObject_CallMethod(_numpy_internal, "_newnames",
                                       "OO", saved, order);
        Py_DECREF(_numpy_internal);
        if (new_name == NULL) {
            return NULL;
        }
        newd = PyArray_DescrNew(saved);
        Py_DECREF(newd->names);
        newd->names = new_name;
        ((PyArrayObject_fields *)self)->descr = newd;
    }

    res = PyArray_ArgSort(self, axis, sortkind);
    if (order != NULL) {
        Py_XDECREF(PyArray_DESCR(self));
        ((PyArrayObject_fields *)self)->descr = saved;
    }
    return PyArray_Return((PyArrayObject *)res);
}


static PyObject *
array_argpartition(PyArrayObject *self,
        PyObject *const *args, Py_ssize_t len_args, PyObject *kwnames)
{
    int axis = -1;
    NPY_SELECTKIND sortkind = NPY_INTROSELECT;
    PyObject *order = NULL, *res;
    PyArray_Descr *newd, *saved=NULL;
    PyObject * kthobj;
    PyArrayObject * ktharray;
    NPY_PREPARE_ARGPARSER;

    if (npy_parse_arguments("argpartition", args, len_args, kwnames,
            "kth", NULL, &kthobj,
            "|axis", &PyArray_AxisConverter, &axis,
            "|kind", &PyArray_SelectkindConverter, &sortkind,
            "|order", NULL, &order,
            NULL, NULL, NULL) < 0) {
        return NULL;
    }
    if (order == Py_None) {
        order = NULL;
    }
    if (order != NULL) {
        PyObject *new_name;
        PyObject *_numpy_internal;
        saved = PyArray_DESCR(self);
        if (!PyDataType_HASFIELDS(saved)) {
            PyErr_SetString(PyExc_ValueError, "Cannot specify "
                            "order when the array has no fields.");
            return NULL;
        }
        _numpy_internal = PyImport_ImportModule("numpy.core._internal");
        if (_numpy_internal == NULL) {
            return NULL;
        }
        new_name = PyObject_CallMethod(_numpy_internal, "_newnames",
                                       "OO", saved, order);
        Py_DECREF(_numpy_internal);
        if (new_name == NULL) {
            return NULL;
        }
        newd = PyArray_DescrNew(saved);
        Py_DECREF(newd->names);
        newd->names = new_name;
        ((PyArrayObject_fields *)self)->descr = newd;
    }

    ktharray = (PyArrayObject *)PyArray_FromAny(kthobj, NULL, 0, 1,
                                                NPY_ARRAY_DEFAULT, NULL);
    if (ktharray == NULL)
        return NULL;

    res = PyArray_ArgPartition(self, ktharray, axis, sortkind);
    Py_DECREF(ktharray);

    if (order != NULL) {
        Py_XDECREF(PyArray_DESCR(self));
        ((PyArrayObject_fields *)self)->descr = saved;
    }
    return PyArray_Return((PyArrayObject *)res);
}

static PyObject *
array_searchsorted(PyArrayObject *self,
        PyObject *const *args, Py_ssize_t len_args, PyObject *kwnames)
{
    PyObject *keys;
    PyObject *sorter;
    NPY_SEARCHSIDE side = NPY_SEARCHLEFT;
    NPY_PREPARE_ARGPARSER;

    sorter = NULL;
    if (npy_parse_arguments("searchsorted", args, len_args, kwnames,
            "v", NULL, &keys,
            "|side", &PyArray_SearchsideConverter, &side,
            "|sorter", NULL, &sorter,
            NULL, NULL, NULL) < 0) {
        return NULL;
    }
    if (sorter == Py_None) {
        sorter = NULL;
    }
    return PyArray_Return((PyArrayObject *)PyArray_SearchSorted(self, keys, side, sorter));
}

static void
_deepcopy_call(char *iptr, char *optr, PyArray_Descr *dtype,
               PyObject *deepcopy, PyObject *visit)
{
    if (!PyDataType_REFCHK(dtype)) {
        return;
    }
    else if (PyDataType_HASFIELDS(dtype)) {
        PyObject *key, *value, *title = NULL;
        PyArray_Descr *new;
        int offset;
        Py_ssize_t pos = 0;
        while (PyDict_Next(dtype->fields, &pos, &key, &value)) {
            if (NPY_TITLE_KEY(key, value)) {
                continue;
            }
            if (!PyArg_ParseTuple(value, "Oi|O", &new, &offset,
                                  &title)) {
                return;
            }
            _deepcopy_call(iptr + offset, optr + offset, new,
                           deepcopy, visit);
        }
    }
    else {
        PyObject *itemp, *otemp;
        PyObject *res;
        memcpy(&itemp, iptr, sizeof(itemp));
        memcpy(&otemp, optr, sizeof(otemp));
        Py_XINCREF(itemp);
        /* call deepcopy on this argument */
        res = PyObject_CallFunctionObjArgs(deepcopy, itemp, visit, NULL);
        Py_XDECREF(itemp);
        Py_XDECREF(otemp);
        memcpy(optr, &res, sizeof(res));
    }

}


static PyObject *
array_deepcopy(PyArrayObject *self, PyObject *args)
{
    PyArrayObject *copied_array;
    PyObject *visit;
    NpyIter *iter;
    NpyIter_IterNextFunc *iternext;
    char *data;
    char **dataptr;
    npy_intp *strideptr, *innersizeptr;
    npy_intp stride, count;
    PyObject *copy, *deepcopy;

    if (!PyArg_ParseTuple(args, "O:__deepcopy__", &visit)) {
        return NULL;
    }
    copied_array = (PyArrayObject*) PyArray_NewCopy(self, NPY_KEEPORDER);
    if (copied_array == NULL) {
        return NULL;
    }
    if (PyDataType_REFCHK(PyArray_DESCR(self))) {
        copy = PyImport_ImportModule("copy");
        if (copy == NULL) {
            Py_DECREF(copied_array);
            return NULL;
        }
        deepcopy = PyObject_GetAttrString(copy, "deepcopy");
        Py_DECREF(copy);
        if (deepcopy == NULL) {
            Py_DECREF(copied_array);
            return NULL;
        }
        iter = NpyIter_New(copied_array,
                           NPY_ITER_READWRITE |
                           NPY_ITER_EXTERNAL_LOOP |
                           NPY_ITER_REFS_OK |
                           NPY_ITER_ZEROSIZE_OK,
                           NPY_KEEPORDER, NPY_NO_CASTING,
                           NULL);
        if (iter == NULL) {
            Py_DECREF(deepcopy);
            Py_DECREF(copied_array);
            return NULL;
        }
        if (NpyIter_GetIterSize(iter) != 0) {
            iternext = NpyIter_GetIterNext(iter, NULL);
            if (iternext == NULL) {
                NpyIter_Deallocate(iter);
                Py_DECREF(deepcopy);
                Py_DECREF(copied_array);
                return NULL;
            }

            dataptr = NpyIter_GetDataPtrArray(iter);
            strideptr = NpyIter_GetInnerStrideArray(iter);
            innersizeptr = NpyIter_GetInnerLoopSizePtr(iter);

            do {
                data = *dataptr;
                stride = *strideptr;
                count = *innersizeptr;
                while (count--) {
                    _deepcopy_call(data, data, PyArray_DESCR(copied_array),
                                   deepcopy, visit);
                    data += stride;
                }
            } while (iternext(iter));
        }
        NpyIter_Deallocate(iter);
        Py_DECREF(deepcopy);
    }
    return (PyObject*) copied_array;
}

/* Convert Array to flat list (using getitem) */
static PyObject *
_getlist_pkl(PyArrayObject *self)
{
    PyObject *theobject;
    PyArrayIterObject *iter = NULL;
    PyObject *list;
    PyArray_GetItemFunc *getitem;

    getitem = PyArray_DESCR(self)->f->getitem;
    iter = (PyArrayIterObject *)PyArray_IterNew((PyObject *)self);
    if (iter == NULL) {
        return NULL;
    }
    list = PyList_New(iter->size);
    if (list == NULL) {
        Py_DECREF(iter);
        return NULL;
    }
    while (iter->index < iter->size) {
        theobject = getitem(iter->dataptr, self);
        PyList_SET_ITEM(list, iter->index, theobject);
        PyArray_ITER_NEXT(iter);
    }
    Py_DECREF(iter);
    return list;
}

static int
_setlist_pkl(PyArrayObject *self, PyObject *list)
{
    PyObject *theobject;
    PyArrayIterObject *iter = NULL;
    PyArray_SetItemFunc *setitem;

    setitem = PyArray_DESCR(self)->f->setitem;
    iter = (PyArrayIterObject *)PyArray_IterNew((PyObject *)self);
    if (iter == NULL) {
        return -1;
    }
    while(iter->index < iter->size) {
        theobject = PyList_GET_ITEM(list, iter->index);
        setitem(theobject, iter->dataptr, self);
        PyArray_ITER_NEXT(iter);
    }
    Py_XDECREF(iter);
    return 0;
}


static PyObject *
array_reduce(PyArrayObject *self, PyObject *NPY_UNUSED(args))
{
    /* version number of this pickle type. Increment if we need to
       change the format. Be sure to handle the old versions in
       array_setstate. */
    const int version = 1;
    PyObject *ret = NULL, *state = NULL, *obj = NULL, *mod = NULL;
    PyObject *mybool, *thestr = NULL;
    PyArray_Descr *descr;

    /* Return a tuple of (callable object, arguments, object's state) */
    /*  We will put everything in the object's state, so that on UnPickle
        it can use the string object as memory without a copy */

    ret = PyTuple_New(3);
    if (ret == NULL) {
        return NULL;
    }
    mod = PyImport_ImportModule("numpy.core._multiarray_umath");
    if (mod == NULL) {
        Py_DECREF(ret);
        return NULL;
    }
    obj = PyObject_GetAttrString(mod, "_reconstruct");
    Py_DECREF(mod);
    PyTuple_SET_ITEM(ret, 0, obj);
    PyTuple_SET_ITEM(ret, 1,
                     Py_BuildValue("ONc",
                                   (PyObject *)Py_TYPE(self),
                                   Py_BuildValue("(N)",
                                                 PyLong_FromLong(0)),
                                   /* dummy data-type */
                                   'b'));

    /* Now fill in object's state.  This is a tuple with
       5 arguments

       1) an integer with the pickle version.
       2) a Tuple giving the shape
       3) a PyArray_Descr Object (with correct bytorder set)
       4) a npy_bool stating if Fortran or not
       5) a Python object representing the data (a string, or
       a list or any user-defined object).

       Notice because Python does not describe a mechanism to write
       raw data to the pickle, this performs a copy to a string first
       This issue is now addressed in protocol 5, where a buffer is serialized
       instead of a string,
    */

    state = PyTuple_New(5);
    if (state == NULL) {
        Py_DECREF(ret);
        return NULL;
    }
    PyTuple_SET_ITEM(state, 0, PyLong_FromLong(version));
    PyTuple_SET_ITEM(state, 1, PyObject_GetAttrString((PyObject *)self,
                                                      "shape"));
    descr = PyArray_DESCR(self);
    Py_INCREF(descr);
    PyTuple_SET_ITEM(state, 2, (PyObject *)descr);
    mybool = (PyArray_ISFORTRAN(self) ? Py_True : Py_False);
    Py_INCREF(mybool);
    PyTuple_SET_ITEM(state, 3, mybool);
    if (PyDataType_FLAGCHK(PyArray_DESCR(self), NPY_LIST_PICKLE)) {
        thestr = _getlist_pkl(self);
    }
    else {
        thestr = PyArray_ToString(self, NPY_ANYORDER);
    }
    if (thestr == NULL) {
        Py_DECREF(ret);
        Py_DECREF(state);
        return NULL;
    }
    PyTuple_SET_ITEM(state, 4, thestr);
    PyTuple_SET_ITEM(ret, 2, state);
    return ret;
}

static PyObject *
array_reduce_ex_regular(PyArrayObject *self, int NPY_UNUSED(protocol))
{
    PyObject *subclass_array_reduce = NULL;
    PyObject *ret;

    /* We do not call array_reduce directly but instead lookup and call
     * the __reduce__ method to make sure that it's possible to customize
     * pickling in sub-classes. */
    subclass_array_reduce = PyObject_GetAttrString((PyObject *)self,
                                                   "__reduce__");
    if (subclass_array_reduce == NULL) {
        return NULL;
    }
    ret = PyObject_CallObject(subclass_array_reduce, NULL);
    Py_DECREF(subclass_array_reduce);
    return ret;
}

static PyObject *
array_reduce_ex_picklebuffer(PyArrayObject *self, int protocol)
{
    PyObject *numeric_mod = NULL, *from_buffer_func = NULL;
    PyObject *pickle_module = NULL, *picklebuf_class = NULL;
    PyObject *picklebuf_args = NULL;
    PyObject *buffer = NULL, *transposed_array = NULL;
    PyArray_Descr *descr = NULL;
    char order;

    descr = PyArray_DESCR(self);

    /* if the python version is below 3.8, the pickle module does not provide
     * built-in support for protocol 5. We try importing the pickle5
     * backport instead */
#if PY_VERSION_HEX >= 0x03080000
    /* we expect protocol 5 to be available in Python 3.8 */
    pickle_module = PyImport_ImportModule("pickle");
#else
    pickle_module = PyImport_ImportModule("pickle5");
    if (pickle_module == NULL) {
        /* for protocol 5, raise a clear ImportError if pickle5 is not found
         */
        PyErr_SetString(PyExc_ImportError, "Using pickle protocol 5 "
                "requires the pickle5 module for Python >=3.6 and <3.8");
        return NULL;
    }
#endif
    if (pickle_module == NULL){
        return NULL;
    }
    picklebuf_class = PyObject_GetAttrString(pickle_module, "PickleBuffer");
    Py_DECREF(pickle_module);
    if (picklebuf_class == NULL) {
        return NULL;
    }

    /* Construct a PickleBuffer of the array */

    if (!PyArray_IS_C_CONTIGUOUS((PyArrayObject*) self) &&
         PyArray_IS_F_CONTIGUOUS((PyArrayObject*) self)) {
        /* if the array if Fortran-contiguous and not C-contiguous,
         * the PickleBuffer instance will hold a view on the transpose
         * of the initial array, that is C-contiguous. */
        order = 'F';
        transposed_array = PyArray_Transpose((PyArrayObject*)self, NULL);
        picklebuf_args = Py_BuildValue("(N)", transposed_array);
    }
    else {
        order = 'C';
        picklebuf_args = Py_BuildValue("(O)", self);
    }
    if (picklebuf_args == NULL) {
        Py_DECREF(picklebuf_class);
        return NULL;
    }

    buffer = PyObject_CallObject(picklebuf_class, picklebuf_args);
    Py_DECREF(picklebuf_class);
    Py_DECREF(picklebuf_args);
    if (buffer == NULL) {
        /* Some arrays may refuse to export a buffer, in which case
         * just fall back on regular __reduce_ex__ implementation
         * (gh-12745).
         */
        PyErr_Clear();
        return array_reduce_ex_regular(self, protocol);
    }

    /* Get the _frombuffer() function for reconstruction */

    numeric_mod = PyImport_ImportModule("numpy.core.numeric");
    if (numeric_mod == NULL) {
        Py_DECREF(buffer);
        return NULL;
    }
    from_buffer_func = PyObject_GetAttrString(numeric_mod,
                                              "_frombuffer");
    Py_DECREF(numeric_mod);
    if (from_buffer_func == NULL) {
        Py_DECREF(buffer);
        return NULL;
    }

    return Py_BuildValue("N(NONN)",
                         from_buffer_func, buffer, (PyObject *)descr,
                         PyObject_GetAttrString((PyObject *)self, "shape"),
                         PyUnicode_FromStringAndSize(&order, 1));
}

static PyObject *
array_reduce_ex(PyArrayObject *self, PyObject *args)
{
    int protocol;
    PyArray_Descr *descr = NULL;

    if (!PyArg_ParseTuple(args, "i", &protocol)) {
        return NULL;
    }

    descr = PyArray_DESCR(self);
    if ((protocol < 5) ||
        (!PyArray_IS_C_CONTIGUOUS((PyArrayObject*)self) &&
         !PyArray_IS_F_CONTIGUOUS((PyArrayObject*)self)) ||
        PyDataType_FLAGCHK(descr, NPY_ITEM_HASOBJECT) ||
        (PyType_IsSubtype(((PyObject*)self)->ob_type, &PyArray_Type) &&
         ((PyObject*)self)->ob_type != &PyArray_Type) ||
        descr->elsize == 0) {
        /* The PickleBuffer class from version 5 of the pickle protocol
         * can only be used for arrays backed by a contiguous data buffer.
         * For all other cases we fallback to the generic array_reduce
         * method that involves using a temporary bytes allocation. */
        return array_reduce_ex_regular(self, protocol);
    }
    else {
        return array_reduce_ex_picklebuffer(self, protocol);
    }
}

static PyObject *
array_setstate(PyArrayObject *self, PyObject *args)
{
    PyObject *shape;
    PyArray_Descr *typecode;
    int version = 1;
    int is_f_order;
    PyObject *rawdata = NULL;
    char *datastr;
    Py_ssize_t len;
    npy_intp size, dimensions[NPY_MAXDIMS];
    int nd;
    npy_intp nbytes;
    int overflowed;

    PyArrayObject_fields *fa = (PyArrayObject_fields *)self;

    /* This will free any memory associated with a and
       use the string in setstate as the (writeable) memory.
    */
    if (!PyArg_ParseTuple(args, "(iO!O!iO):__setstate__",
                            &version,
                            &PyTuple_Type, &shape,
                            &PyArrayDescr_Type, &typecode,
                            &is_f_order,
                            &rawdata)) {
        PyErr_Clear();
        version = 0;
        if (!PyArg_ParseTuple(args, "(O!O!iO):__setstate__",
                            &PyTuple_Type, &shape,
                            &PyArrayDescr_Type, &typecode,
                            &is_f_order,
                            &rawdata)) {
            return NULL;
        }
    }

    /* If we ever need another pickle format, increment the version
       number. But we should still be able to handle the old versions.
       We've only got one right now. */
    if (version != 1 && version != 0) {
        PyErr_Format(PyExc_ValueError,
                     "can't handle version %d of numpy.ndarray pickle",
                     version);
        return NULL;
    }

    Py_XDECREF(PyArray_DESCR(self));
    fa->descr = typecode;
    Py_INCREF(typecode);
    nd = PyArray_IntpFromSequence(shape, dimensions, NPY_MAXDIMS);
    if (nd < 0) {
        return NULL;
    }
    size = PyArray_MultiplyList(dimensions, nd);
    if (size < 0) {
        /* More items than are addressable */
        return PyErr_NoMemory();
    }
    overflowed = npy_mul_with_overflow_intp(
        &nbytes, size, PyArray_DESCR(self)->elsize);
    if (overflowed) {
        /* More bytes than are addressable */
        return PyErr_NoMemory();
    }

    if (PyDataType_FLAGCHK(typecode, NPY_LIST_PICKLE)) {
        if (!PyList_Check(rawdata)) {
            PyErr_SetString(PyExc_TypeError,
                            "object pickle not returning list");
            return NULL;
        }
    }
    else {
        Py_INCREF(rawdata);

        /* Backward compatibility with Python 2 NumPy pickles */
        if (PyUnicode_Check(rawdata)) {
            PyObject *tmp;
            tmp = PyUnicode_AsLatin1String(rawdata);
            Py_DECREF(rawdata);
            rawdata = tmp;
            if (tmp == NULL) {
                /* More informative error message */
                PyErr_SetString(PyExc_ValueError,
                                ("Failed to encode latin1 string when unpickling a Numpy array. "
                                 "pickle.load(a, encoding='latin1') is assumed."));
                return NULL;
            }
        }

        if (!PyBytes_Check(rawdata)) {
            PyErr_SetString(PyExc_TypeError,
                            "pickle not returning string");
            Py_DECREF(rawdata);
            return NULL;
        }

        if (PyBytes_AsStringAndSize(rawdata, &datastr, &len) < 0) {
            Py_DECREF(rawdata);
            return NULL;
        }

        if (len != nbytes) {
            PyErr_SetString(PyExc_ValueError,
                            "buffer size does not"  \
                            " match array size");
            Py_DECREF(rawdata);
            return NULL;
        }
    }

    if ((PyArray_FLAGS(self) & NPY_ARRAY_OWNDATA)) {
        PyDataMem_FREE(PyArray_DATA(self));
        PyArray_CLEARFLAGS(self, NPY_ARRAY_OWNDATA);
    }
    Py_XDECREF(PyArray_BASE(self));
    fa->base = NULL;

    PyArray_CLEARFLAGS(self, NPY_ARRAY_WRITEBACKIFCOPY);
    PyArray_CLEARFLAGS(self, NPY_ARRAY_UPDATEIFCOPY);

    if (PyArray_DIMS(self) != NULL) {
        npy_free_cache_dim_array(self);
        fa->dimensions = NULL;
    }

    fa->flags = NPY_ARRAY_DEFAULT;

    fa->nd = nd;

    if (nd > 0) {
        fa->dimensions = npy_alloc_cache_dim(2 * nd);
        if (fa->dimensions == NULL) {
            return PyErr_NoMemory();
        }
        fa->strides = PyArray_DIMS(self) + nd;
        if (nd) {
            memcpy(PyArray_DIMS(self), dimensions, sizeof(npy_intp)*nd);
        }
        _array_fill_strides(PyArray_STRIDES(self), dimensions, nd,
                               PyArray_DESCR(self)->elsize,
                               (is_f_order ? NPY_ARRAY_F_CONTIGUOUS :
                                             NPY_ARRAY_C_CONTIGUOUS),
                               &(fa->flags));
    }

    if (!PyDataType_FLAGCHK(typecode, NPY_LIST_PICKLE)) {
        int swap = PyArray_ISBYTESWAPPED(self);
        fa->data = datastr;
        /* Bytes should always be considered immutable, but we just grab the
         * pointer if they are large, to save memory. */
        if (!IsAligned(self) || swap || (len <= 1000)) {
            npy_intp num = PyArray_NBYTES(self);
            if (num == 0) {
                Py_DECREF(rawdata);
                Py_RETURN_NONE;
            }
            fa->data = PyDataMem_NEW(num);
            if (PyArray_DATA(self) == NULL) {
                Py_DECREF(rawdata);
                return PyErr_NoMemory();
            }
            if (swap) {
                /* byte-swap on pickle-read */
                npy_intp numels = PyArray_SIZE(self);
                PyArray_DESCR(self)->f->copyswapn(PyArray_DATA(self),
                                        PyArray_DESCR(self)->elsize,
                                        datastr, PyArray_DESCR(self)->elsize,
                                        numels, 1, self);
                if (!(PyArray_ISEXTENDED(self) ||
                      PyArray_DESCR(self)->metadata ||
                      PyArray_DESCR(self)->c_metadata)) {
                    fa->descr = PyArray_DescrFromType(
                                    PyArray_DESCR(self)->type_num);
                }
                else {
                    fa->descr = PyArray_DescrNew(typecode);
                    if (PyArray_DESCR(self)->byteorder == NPY_BIG) {
                        PyArray_DESCR(self)->byteorder = NPY_LITTLE;
                    }
                    else if (PyArray_DESCR(self)->byteorder == NPY_LITTLE) {
                        PyArray_DESCR(self)->byteorder = NPY_BIG;
                    }
                }
                Py_DECREF(typecode);
            }
            else {
                memcpy(PyArray_DATA(self), datastr, num);
            }
            PyArray_ENABLEFLAGS(self, NPY_ARRAY_OWNDATA);
            fa->base = NULL;
            Py_DECREF(rawdata);
        }
        else {
            if (PyArray_SetBaseObject(self, rawdata) < 0) {
                return NULL;
            }
        }
    }
    else {
        npy_intp num = PyArray_NBYTES(self);
        int elsize = PyArray_DESCR(self)->elsize;
        if (num == 0 || elsize == 0) {
            Py_RETURN_NONE;
        }
        fa->data = PyDataMem_NEW(num);
        if (PyArray_DATA(self) == NULL) {
            return PyErr_NoMemory();
        }
        if (PyDataType_FLAGCHK(PyArray_DESCR(self), NPY_NEEDS_INIT)) {
            memset(PyArray_DATA(self), 0, PyArray_NBYTES(self));
        }
        PyArray_ENABLEFLAGS(self, NPY_ARRAY_OWNDATA);
        fa->base = NULL;
        if (_setlist_pkl(self, rawdata) < 0) {
            return NULL;
        }
    }

    PyArray_UpdateFlags(self, NPY_ARRAY_UPDATE_ALL);

    Py_RETURN_NONE;
}

/*NUMPY_API*/
NPY_NO_EXPORT int
PyArray_Dump(PyObject *self, PyObject *file, int protocol)
{
    static PyObject *method = NULL;
    PyObject *ret;
    npy_cache_import("numpy.core._methods", "_dump", &method);
    if (method == NULL) {
        return -1;
    }
    if (protocol < 0) {
        ret = PyObject_CallFunction(method, "OO", self, file);
    }
    else {
        ret = PyObject_CallFunction(method, "OOi", self, file, protocol);
    }
    if (ret == NULL) {
        return -1;
    }
    Py_DECREF(ret);
    return 0;
}

/*NUMPY_API*/
NPY_NO_EXPORT PyObject *
PyArray_Dumps(PyObject *self, int protocol)
{
    static PyObject *method = NULL;
    npy_cache_import("numpy.core._methods", "_dumps", &method);
    if (method == NULL) {
        return NULL;
    }
    if (protocol < 0) {
        return PyObject_CallFunction(method, "O", self);
    }
    else {
        return PyObject_CallFunction(method, "Oi", self, protocol);
    }
}


static PyObject *
array_dump(PyArrayObject *self, PyObject *args, PyObject *kwds)
{
    NPY_FORWARD_NDARRAY_METHOD("_dump");
}


static PyObject *
array_dumps(PyArrayObject *self, PyObject *args, PyObject *kwds)
{
    NPY_FORWARD_NDARRAY_METHOD("_dumps");
}


static PyObject *
array_sizeof(PyArrayObject *self)
{
    /* object + dimension and strides */
    Py_ssize_t nbytes = Py_TYPE(self)->tp_basicsize +
        PyArray_NDIM(self) * sizeof(npy_intp) * 2;
    if (PyArray_CHKFLAGS(self, NPY_ARRAY_OWNDATA)) {
        nbytes += PyArray_NBYTES(self);
    }
    return PyLong_FromSsize_t(nbytes);
}


static PyObject *
array_transpose(PyArrayObject *self, PyObject *args)
{
    PyObject *shape = Py_None;
    Py_ssize_t n = PyTuple_Size(args);
    PyArray_Dims permute;
    PyObject *ret;

    if (n > 1) {
        shape = args;
    }
    else if (n == 1) {
        shape = PyTuple_GET_ITEM(args, 0);
    }

    if (shape == Py_None) {
        ret = PyArray_Transpose(self, NULL);
    }
    else {
        if (!PyArray_IntpConverter(shape, &permute)) {
            return NULL;
        }
        ret = PyArray_Transpose(self, &permute);
        npy_free_cache_dim_obj(permute);
    }

    return ret;
}

#define _CHKTYPENUM(typ) ((typ) ? (typ)->type_num : NPY_NOTYPE)

static PyObject *
array_mean(PyArrayObject *self, PyObject *args, PyObject *kwds)
{
    NPY_FORWARD_NDARRAY_METHOD("_mean");
}

static PyObject *
array_sum(PyArrayObject *self, PyObject *args, PyObject *kwds)
{
    NPY_FORWARD_NDARRAY_METHOD("_sum");
}


static PyObject *
array_cumsum(PyArrayObject *self, PyObject *args, PyObject *kwds)
{
    int axis = NPY_MAXDIMS;
    PyArray_Descr *dtype = NULL;
    PyArrayObject *out = NULL;
    int rtype;
    static char *kwlist[] = {"axis", "dtype", "out", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O&O&O&:cumsum", kwlist,
                                     PyArray_AxisConverter, &axis,
                                     PyArray_DescrConverter2, &dtype,
                                     PyArray_OutputConverter, &out)) {
        Py_XDECREF(dtype);
        return NULL;
    }

    rtype = _CHKTYPENUM(dtype);
    Py_XDECREF(dtype);
    return PyArray_CumSum(self, axis, rtype, out);
}

static PyObject *
array_prod(PyArrayObject *self, PyObject *args, PyObject *kwds)
{
    NPY_FORWARD_NDARRAY_METHOD("_prod");
}

static PyObject *
array_cumprod(PyArrayObject *self, PyObject *args, PyObject *kwds)
{
    int axis = NPY_MAXDIMS;
    PyArray_Descr *dtype = NULL;
    PyArrayObject *out = NULL;
    int rtype;
    static char *kwlist[] = {"axis", "dtype", "out", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O&O&O&:cumprod", kwlist,
                                     PyArray_AxisConverter, &axis,
                                     PyArray_DescrConverter2, &dtype,
                                     PyArray_OutputConverter, &out)) {
        Py_XDECREF(dtype);
        return NULL;
    }

    rtype = _CHKTYPENUM(dtype);
    Py_XDECREF(dtype);
    return PyArray_CumProd(self, axis, rtype, out);
}


static PyObject *
array_dot(PyArrayObject *self,
        PyObject *const *args, Py_ssize_t len_args, PyObject *kwnames)
{
    PyObject *a = (PyObject *)self, *b, *o = NULL;
    PyArrayObject *ret;
    NPY_PREPARE_ARGPARSER;

    if (npy_parse_arguments("dot", args, len_args, kwnames,
            "b", NULL, &b,
            "|out", NULL, &o,
            NULL, NULL, NULL) < 0) {
        return NULL;
    }

    if (o != NULL) {
        if (o == Py_None) {
            o = NULL;
        }
        else if (!PyArray_Check(o)) {
            PyErr_SetString(PyExc_TypeError,
                            "'out' must be an array");
            return NULL;
        }
    }
    ret = (PyArrayObject *)PyArray_MatrixProduct2(a, b, (PyArrayObject *)o);
    return PyArray_Return(ret);
}


static PyObject *
array_any(PyArrayObject *self, PyObject *args, PyObject *kwds)
{
    NPY_FORWARD_NDARRAY_METHOD("_any");
}


static PyObject *
array_all(PyArrayObject *self, PyObject *args, PyObject *kwds)
{
    NPY_FORWARD_NDARRAY_METHOD("_all");
}

static PyObject *
array_stddev(PyArrayObject *self, PyObject *args, PyObject *kwds)
{
    NPY_FORWARD_NDARRAY_METHOD("_std");
}

static PyObject *
array_variance(PyArrayObject *self, PyObject *args, PyObject *kwds)
{
    NPY_FORWARD_NDARRAY_METHOD("_var");
}

static PyObject *
array_compress(PyArrayObject *self, PyObject *args, PyObject *kwds)
{
    int axis = NPY_MAXDIMS;
    PyObject *condition;
    PyArrayObject *out = NULL;
    static char *kwlist[] = {"condition", "axis", "out", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O&O&:compress", kwlist,
                                     &condition,
                                     PyArray_AxisConverter, &axis,
                                     PyArray_OutputConverter, &out)) {
        return NULL;
    }

    PyObject *ret = PyArray_Compress(self, condition, axis, out);

    /* this matches the unpacking behavior of ufuncs */
    if (out == NULL) {
        return PyArray_Return((PyArrayObject *)ret);
    }
    else {
        return ret;
    }
}


static PyObject *
array_nonzero(PyArrayObject *self, PyObject *args)
{
    if (!PyArg_ParseTuple(args, "")) {
        return NULL;
    }
    return PyArray_Nonzero(self);
}


static PyObject *
array_trace(PyArrayObject *self,
        PyObject *const *args, Py_ssize_t len_args, PyObject *kwnames)
{
    int axis1 = 0, axis2 = 1, offset = 0;
    PyArray_Descr *dtype = NULL;
    PyArrayObject *out = NULL;
    int rtype;
    NPY_PREPARE_ARGPARSER;

    if (npy_parse_arguments("trace", args, len_args, kwnames,
            "|offset", &PyArray_PythonPyIntFromInt, &offset,
            "|axis1", &PyArray_PythonPyIntFromInt, &axis1,
            "|axis2", &PyArray_PythonPyIntFromInt, &axis2,
            "|dtype", &PyArray_DescrConverter2, &dtype,
            "|out", &PyArray_OutputConverter, &out,
            NULL, NULL, NULL) < 0) {
        Py_XDECREF(dtype);
        return NULL;
    }

    rtype = _CHKTYPENUM(dtype);
    Py_XDECREF(dtype);
    PyObject *ret = PyArray_Trace(self, offset, axis1, axis2, rtype, out);

    /* this matches the unpacking behavior of ufuncs */
    if (out == NULL) {
        return PyArray_Return((PyArrayObject *)ret);
    }
    else {
        return ret;
    }
}

#undef _CHKTYPENUM


static PyObject *
array_clip(PyArrayObject *self, PyObject *args, PyObject *kwds)
{
    NPY_FORWARD_NDARRAY_METHOD("_clip");
}


static PyObject *
array_conjugate(PyArrayObject *self, PyObject *args)
{
    PyArrayObject *out = NULL;
    if (!PyArg_ParseTuple(args, "|O&:conjugate",
                          PyArray_OutputConverter,
                          &out)) {
        return NULL;
    }
    return PyArray_Conjugate(self, out);
}


static PyObject *
array_diagonal(PyArrayObject *self, PyObject *args, PyObject *kwds)
{
    int axis1 = 0, axis2 = 1, offset = 0;
    static char *kwlist[] = {"offset", "axis1", "axis2", NULL};
    PyArrayObject *ret;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|iii:diagonal", kwlist,
                                     &offset,
                                     &axis1,
                                     &axis2)) {
        return NULL;
    }

    ret = (PyArrayObject *)PyArray_Diagonal(self, offset, axis1, axis2);
    return PyArray_Return(ret);
}


static PyObject *
array_flatten(PyArrayObject *self,
        PyObject *const *args, Py_ssize_t len_args, PyObject *kwnames)
{
    NPY_ORDER order = NPY_CORDER;
    NPY_PREPARE_ARGPARSER;

    if (npy_parse_arguments("flatten", args, len_args, kwnames,
            "|order", PyArray_OrderConverter, &order,
            NULL, NULL, NULL) < 0) {
        return NULL;
    }
    return PyArray_Flatten(self, order);
}


static PyObject *
array_ravel(PyArrayObject *self,
        PyObject *const *args, Py_ssize_t len_args, PyObject *kwnames)
{
    NPY_ORDER order = NPY_CORDER;
    NPY_PREPARE_ARGPARSER;

    if (npy_parse_arguments("ravel", args, len_args, kwnames,
            "|order", PyArray_OrderConverter, &order,
            NULL, NULL, NULL) < 0) {
        return NULL;
    }
    return PyArray_Ravel(self, order);
}


static PyObject *
array_round(PyArrayObject *self, PyObject *args, PyObject *kwds)
{
    int decimals = 0;
    PyArrayObject *out = NULL;
    static char *kwlist[] = {"decimals", "out", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|iO&:round", kwlist,
                                     &decimals,
                                     PyArray_OutputConverter, &out)) {
        return NULL;
    }

    PyObject *ret = PyArray_Round(self, decimals, out);

    /* this matches the unpacking behavior of ufuncs */
    if (out == NULL) {
        return PyArray_Return((PyArrayObject *)ret);
    }
    else {
        return ret;
    }
}



static PyObject *
array_setflags(PyArrayObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"write", "align", "uic", NULL};
    PyObject *write_flag = Py_None;
    PyObject *align_flag = Py_None;
    PyObject *uic = Py_None;
    int flagback = PyArray_FLAGS(self);

    PyArrayObject_fields *fa = (PyArrayObject_fields *)self;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|OOO:setflags", kwlist,
                                     &write_flag,
                                     &align_flag,
                                     &uic))
        return NULL;

    if (align_flag != Py_None) {
        if (PyObject_Not(align_flag)) {
            PyArray_CLEARFLAGS(self, NPY_ARRAY_ALIGNED);
        }
        else if (IsAligned(self)) {
            PyArray_ENABLEFLAGS(self, NPY_ARRAY_ALIGNED);
        }
        else {
            PyErr_SetString(PyExc_ValueError,
                            "cannot set aligned flag of mis-"
                            "aligned array to True");
            return NULL;
        }
    }

    if (uic != Py_None) {
        if (PyObject_IsTrue(uic)) {
            fa->flags = flagback;
            PyErr_SetString(PyExc_ValueError,
                            "cannot set WRITEBACKIFCOPY "
                            "flag to True");
            return NULL;
        }
        else {
            PyArray_CLEARFLAGS(self, NPY_ARRAY_WRITEBACKIFCOPY);
            PyArray_CLEARFLAGS(self, NPY_ARRAY_UPDATEIFCOPY);
            Py_XDECREF(fa->base);
            fa->base = NULL;
        }
    }

    if (write_flag != Py_None) {
        if (PyObject_IsTrue(write_flag)) {
            if (_IsWriteable(self)) {
                /*
                 * _IsWritable (and PyArray_UpdateFlags) allows flipping this,
                 * although the C-Api user who created the array may have
                 * chosen to make it non-writable for a good reason, so
                 * deprecate.
                 */
                if ((PyArray_BASE(self) == NULL) &&
                            !PyArray_CHKFLAGS(self, NPY_ARRAY_OWNDATA) &&
                            !PyArray_CHKFLAGS(self, NPY_ARRAY_WRITEABLE)) {
                    /* 2017-05-03, NumPy 1.17.0 */
                    if (DEPRECATE("making a non-writeable array writeable "
                                  "is deprecated for arrays without a base "
                                  "which do not own their data.") < 0) {
                        return NULL;
                    }
                }
                PyArray_ENABLEFLAGS(self, NPY_ARRAY_WRITEABLE);
                PyArray_CLEARFLAGS(self, NPY_ARRAY_WARN_ON_WRITE);
            }
            else {
                fa->flags = flagback;
                PyErr_SetString(PyExc_ValueError,
                                "cannot set WRITEABLE "
                                "flag to True of this "
                                "array");
                return NULL;
            }
        }
        else {
            PyArray_CLEARFLAGS(self, NPY_ARRAY_WRITEABLE);
            PyArray_CLEARFLAGS(self, NPY_ARRAY_WARN_ON_WRITE);
        }
    }
    Py_RETURN_NONE;
}


static PyObject *
array_newbyteorder(PyArrayObject *self, PyObject *args)
{
    char endian = NPY_SWAP;
    PyArray_Descr *new;

    if (!PyArg_ParseTuple(args, "|O&:newbyteorder", PyArray_ByteorderConverter,
                          &endian)) {
        return NULL;
    }
    new = PyArray_DescrNewByteorder(PyArray_DESCR(self), endian);
    if (!new) {
        return NULL;
    }
    return PyArray_View(self, new, NULL);

}

static PyObject *
array_complex(PyArrayObject *self, PyObject *NPY_UNUSED(args))
{
    PyArrayObject *arr;
    PyArray_Descr *dtype;
    PyObject *c;

    if (PyArray_SIZE(self) != 1) {
        PyErr_SetString(PyExc_TypeError,
                "only length-1 arrays can be converted to Python scalars");
        return NULL;
    }

    dtype = PyArray_DescrFromType(NPY_CDOUBLE);
    if (dtype == NULL) {
        return NULL;
    }

    if (!PyArray_CanCastArrayTo(self, dtype, NPY_SAME_KIND_CASTING) &&
            !(PyArray_TYPE(self) == NPY_OBJECT)) {
        PyObject *descr = (PyObject*)PyArray_DESCR(self);

        Py_DECREF(dtype);
        PyErr_Format(PyExc_TypeError,
                "Unable to convert %R to complex", descr);
        return NULL;
    }

    if (PyArray_TYPE(self) == NPY_OBJECT) {
        /* let python try calling __complex__ on the object. */
        PyObject *args, *res;

        Py_DECREF(dtype);
        args = Py_BuildValue("(O)", *((PyObject**)PyArray_DATA(self)));
        if (args == NULL) {
            return NULL;
        }
        res = PyComplex_Type.tp_new(&PyComplex_Type, args, NULL);
        Py_DECREF(args);
        return res;
    }

    arr = (PyArrayObject *)PyArray_CastToType(self, dtype, 0);
    if (arr == NULL) {
        return NULL;
    }
    c = PyComplex_FromCComplex(*((Py_complex*)PyArray_DATA(arr)));
    Py_DECREF(arr);
    return c;
}

NPY_NO_EXPORT PyMethodDef array_methods[] = {

    /* for subtypes */
    {"__array__",
        (PyCFunction)array_getarray,
        METH_VARARGS, NULL},
    {"__array_prepare__",
        (PyCFunction)array_preparearray,
        METH_VARARGS, NULL},
    {"__array_wrap__",
        (PyCFunction)array_wraparray,
        METH_VARARGS, NULL},
    {"__array_ufunc__",
        (PyCFunction)array_ufunc,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"__array_function__",
        (PyCFunction)array_function,
        METH_VARARGS | METH_KEYWORDS, NULL},

    /* for the sys module */
    {"__sizeof__",
        (PyCFunction) array_sizeof,
        METH_NOARGS, NULL},

    /* for the copy module */
    {"__copy__",
        (PyCFunction)array_copy_keeporder,
        METH_VARARGS, NULL},
    {"__deepcopy__",
        (PyCFunction)array_deepcopy,
        METH_VARARGS, NULL},

    /* for Pickling */
    {"__reduce__",
        (PyCFunction) array_reduce,
        METH_VARARGS, NULL},
    {"__reduce_ex__",
        (PyCFunction) array_reduce_ex,
        METH_VARARGS, NULL},
    {"__setstate__",
        (PyCFunction) array_setstate,
        METH_VARARGS, NULL},
    {"dumps",
        (PyCFunction) array_dumps,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"dump",
        (PyCFunction) array_dump,
        METH_VARARGS | METH_KEYWORDS, NULL},

    {"__complex__",
        (PyCFunction) array_complex,
        METH_VARARGS, NULL},

    {"__format__",
        (PyCFunction) array_format,
        METH_VARARGS, NULL},

    /* Original and Extended methods added 2005 */
    {"all",
        (PyCFunction)array_all,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"any",
        (PyCFunction)array_any,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"argmax",
        (PyCFunction)array_argmax,
        METH_FASTCALL | METH_KEYWORDS, NULL},
    {"argmin",
        (PyCFunction)array_argmin,
        METH_FASTCALL | METH_KEYWORDS, NULL},
    {"argpartition",
        (PyCFunction)array_argpartition,
        METH_FASTCALL | METH_KEYWORDS, NULL},
    {"argsort",
        (PyCFunction)array_argsort,
        METH_FASTCALL | METH_KEYWORDS, NULL},
    {"astype",
        (PyCFunction)array_astype,
        METH_FASTCALL | METH_KEYWORDS, NULL},
    {"byteswap",
        (PyCFunction)array_byteswap,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"choose",
        (PyCFunction)array_choose,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"clip",
        (PyCFunction)array_clip,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"compress",
        (PyCFunction)array_compress,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"conj",
        (PyCFunction)array_conjugate,
        METH_VARARGS, NULL},
    {"conjugate",
        (PyCFunction)array_conjugate,
        METH_VARARGS, NULL},
    {"copy",
        (PyCFunction)array_copy,
        METH_FASTCALL | METH_KEYWORDS, NULL},
    {"cumprod",
        (PyCFunction)array_cumprod,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"cumsum",
        (PyCFunction)array_cumsum,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"diagonal",
        (PyCFunction)array_diagonal,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"dot",
        (PyCFunction)array_dot,
        METH_FASTCALL | METH_KEYWORDS, NULL},
    {"fill",
        (PyCFunction)array_fill,
        METH_VARARGS, NULL},
    {"flatten",
        (PyCFunction)array_flatten,
        METH_FASTCALL | METH_KEYWORDS, NULL},
    {"getfield",
        (PyCFunction)array_getfield,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"item",
        (PyCFunction)array_toscalar,
        METH_VARARGS, NULL},
    {"itemset",
        (PyCFunction) array_setscalar,
        METH_VARARGS, NULL},
    {"max",
        (PyCFunction)array_max,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"mean",
        (PyCFunction)array_mean,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"min",
        (PyCFunction)array_min,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"newbyteorder",
        (PyCFunction)array_newbyteorder,
        METH_VARARGS, NULL},
    {"nonzero",
        (PyCFunction)array_nonzero,
        METH_VARARGS, NULL},
    {"partition",
        (PyCFunction)array_partition,
        METH_FASTCALL | METH_KEYWORDS, NULL},
    {"prod",
        (PyCFunction)array_prod,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"ptp",
        (PyCFunction)array_ptp,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"put",
        (PyCFunction)array_put,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"ravel",
        (PyCFunction)array_ravel,
        METH_FASTCALL | METH_KEYWORDS, NULL},
    {"repeat",
        (PyCFunction)array_repeat,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"reshape",
        (PyCFunction)array_reshape,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"resize",
        (PyCFunction)array_resize,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"round",
        (PyCFunction)array_round,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"searchsorted",
        (PyCFunction)array_searchsorted,
        METH_FASTCALL | METH_KEYWORDS, NULL},
    {"setfield",
        (PyCFunction)array_setfield,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"setflags",
        (PyCFunction)array_setflags,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"sort",
        (PyCFunction)array_sort,
        METH_FASTCALL | METH_KEYWORDS, NULL},
    {"squeeze",
        (PyCFunction)array_squeeze,
        METH_FASTCALL | METH_KEYWORDS, NULL},
    {"std",
        (PyCFunction)array_stddev,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"sum",
        (PyCFunction)array_sum,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"swapaxes",
        (PyCFunction)array_swapaxes,
        METH_VARARGS, NULL},
    {"take",
        (PyCFunction)array_take,
        METH_FASTCALL | METH_KEYWORDS, NULL},
    {"tobytes",
        (PyCFunction)array_tobytes,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"tofile",
        (PyCFunction)array_tofile,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"tolist",
        (PyCFunction)array_tolist,
        METH_VARARGS, NULL},
    {"tostring",
        (PyCFunction)array_tostring,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"trace",
        (PyCFunction)array_trace,
        METH_FASTCALL | METH_KEYWORDS, NULL},
    {"transpose",
        (PyCFunction)array_transpose,
        METH_VARARGS, NULL},
    {"var",
        (PyCFunction)array_variance,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"view",
        (PyCFunction)array_view,
        METH_FASTCALL | METH_KEYWORDS, NULL},
    {NULL, NULL, 0, NULL}           /* sentinel */
};
