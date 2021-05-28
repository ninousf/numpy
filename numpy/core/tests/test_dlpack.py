import sys
import pytest

import numpy as np
from numpy.testing import assert_array_equal, IS_PYPY


class TestDLPack:
    @pytest.mark.skipif(IS_PYPY, reason="PyPy can't get refcounts.")
    def test_dunder_dlpack_refcount(self):
        x = np.arange(5)
        y = x.__dlpack__()
        assert sys.getrefcount(x) == 3
        del y
        assert sys.getrefcount(x) == 2

    def test_dunder_dlpack_stream(self):
        x = np.arange(5)
        x.__dlpack__(stream=None)

        with pytest.raises(RuntimeError):
            x.__dlpack__(stream=1)

    def test_strides_not_multiple_of_itemsize(self):
        dt = np.dtype([('int', np.int32), ('char', np.int8)])
        y = np.zeros((5,), dtype=dt)
        z = y['int']

        with pytest.raises(RuntimeError):
            np.from_dlpack(z)

    @pytest.mark.skipif(IS_PYPY, reason="PyPy can't get refcounts.")
    def test_from_dlpack_refcount(self):
        x = np.arange(5)
        y = np.from_dlpack(x)
        assert sys.getrefcount(x) == 3
        del y
        assert sys.getrefcount(x) == 2

    @pytest.mark.parametrize("dtype", [
        np.int8, np.int16, np.int32, np.int64,
        np.uint8, np.uint16, np.uint32, np.uint64,
        np.float16, np.float32, np.float64,
        np.complex64, np.complex128
    ])
    def test_dtype_passthrough(self, dtype):
        x = np.arange(5, dtype=dtype)
        y = np.from_dlpack(x)

        assert y.dtype == x.dtype
        assert_array_equal(x, y)

    def test_invalid_dtype(self):
        x = np.asarray(np.datetime64('2021-05-27'))

        with pytest.raises(TypeError):
            np.from_dlpack(x)

    def test_invalid_byte_swapping(self):
        dt = np.dtype('=i8').newbyteorder()
        x = np.arange(5, dtype=dt)

        with pytest.raises(TypeError):
            np.from_dlpack(x)

    def test_non_contiguous(self):
        x = np.arange(25).reshape((5, 5))

        y1 = x[0]
        assert_array_equal(y1, np.from_dlpack(y1))

        y2 = x[:, 0]
        assert_array_equal(y2, np.from_dlpack(y2))

        y3 = x[1, :]
        assert_array_equal(y3, np.from_dlpack(y3))

        y4 = x[1]
        assert_array_equal(y4, np.from_dlpack(y4))

        y5 = np.diagonal(x)
        assert_array_equal(y5, np.from_dlpack(y5))

    @pytest.mark.parametrize("ndim", range(33))
    def test_higher_dims(self, ndim):
        shape = (1,) * ndim
        x = np.zeros(shape, dtype=np.float64, order='C')

        assert shape == np.from_dlpack(x).shape
