from __future__ import division, absolute_import, print_function

import sys
import pytest

import numpy as np
from numpy.ctypeslib import ndpointer, load_library, as_array
from numpy.distutils.misc_util import get_shared_lib_extension
from numpy.testing import assert_, assert_array_equal, assert_raises

try:
    cdll = None
    if hasattr(sys, 'gettotalrefcount'):
        try:
            cdll = load_library('multiarray_d', np.core.multiarray.__file__)
        except OSError:
            pass
    if cdll is None:
        cdll = load_library('multiarray', np.core.multiarray.__file__)
    _HAS_CTYPE = True
except ImportError:
    _HAS_CTYPE = False

class TestLoadLibrary(object):
    @pytest.mark.skipif(not _HAS_CTYPE,
                        reason="ctypes not available in this python")
    @pytest.mark.skipif(sys.platform == 'cygwin',
                        reason="Known to fail on cygwin")
    def test_basic(self):
        try:
            # Should succeed
            load_library('multiarray', np.core.multiarray.__file__)
        except ImportError as e:
            msg = ("ctypes is not available on this python: skipping the test"
                   " (import error was: %s)" % str(e))
            print(msg)

    @pytest.mark.skipif(not _HAS_CTYPE,
                        reason="ctypes not available in this python")
    @pytest.mark.skipif(sys.platform == 'cygwin',
                        reason="Known to fail on cygwin")
    def test_basic2(self):
        # Regression for #801: load_library with a full library name
        # (including extension) does not work.
        try:
            try:
                so = get_shared_lib_extension(is_python_ext=True)
                # Should succeed
                load_library('multiarray%s' % so, np.core.multiarray.__file__)
            except ImportError:
                print("No distutils available, skipping test.")
        except ImportError as e:
            msg = ("ctypes is not available on this python: skipping the test"
                   " (import error was: %s)" % str(e))
            print(msg)

class TestNdpointer(object):
    def test_dtype(self):
        dt = np.intc
        p = ndpointer(dtype=dt)
        assert_(p.from_param(np.array([1], dt)))
        dt = '<i4'
        p = ndpointer(dtype=dt)
        assert_(p.from_param(np.array([1], dt)))
        dt = np.dtype('>i4')
        p = ndpointer(dtype=dt)
        p.from_param(np.array([1], dt))
        assert_raises(TypeError, p.from_param,
                          np.array([1], dt.newbyteorder('swap')))
        dtnames = ['x', 'y']
        dtformats = [np.intc, np.float64]
        dtdescr = {'names': dtnames, 'formats': dtformats}
        dt = np.dtype(dtdescr)
        p = ndpointer(dtype=dt)
        assert_(p.from_param(np.zeros((10,), dt)))
        samedt = np.dtype(dtdescr)
        p = ndpointer(dtype=samedt)
        assert_(p.from_param(np.zeros((10,), dt)))
        dt2 = np.dtype(dtdescr, align=True)
        if dt.itemsize != dt2.itemsize:
            assert_raises(TypeError, p.from_param, np.zeros((10,), dt2))
        else:
            assert_(p.from_param(np.zeros((10,), dt2)))

    def test_ndim(self):
        p = ndpointer(ndim=0)
        assert_(p.from_param(np.array(1)))
        assert_raises(TypeError, p.from_param, np.array([1]))
        p = ndpointer(ndim=1)
        assert_raises(TypeError, p.from_param, np.array(1))
        assert_(p.from_param(np.array([1])))
        p = ndpointer(ndim=2)
        assert_(p.from_param(np.array([[1]])))

    def test_shape(self):
        p = ndpointer(shape=(1, 2))
        assert_(p.from_param(np.array([[1, 2]])))
        assert_raises(TypeError, p.from_param, np.array([[1], [2]]))
        p = ndpointer(shape=())
        assert_(p.from_param(np.array(1)))

    def test_flags(self):
        x = np.array([[1, 2], [3, 4]], order='F')
        p = ndpointer(flags='FORTRAN')
        assert_(p.from_param(x))
        p = ndpointer(flags='CONTIGUOUS')
        assert_raises(TypeError, p.from_param, x)
        p = ndpointer(flags=x.flags.num)
        assert_(p.from_param(x))
        assert_raises(TypeError, p.from_param, np.array([[1, 2], [3, 4]]))

    def test_cache(self):
        a1 = ndpointer(dtype=np.float64)
        a2 = ndpointer(dtype=np.float64)
        assert_(a1 == a2)

class TestAsArray(object):
    @pytest.mark.skipif(not _HAS_CTYPE,
                        reason="ctypes not available on this python installation")
    def test_array(self):
        from ctypes import c_int
        at = c_int * 2
        a = as_array(at(1, 2))
        assert_(a.shape == (2,))
        assert_array_equal(a, np.array([1, 2]))
        a = as_array((at * 3)(at(1, 2), at(3, 4), at(5, 6)))
        assert_(a.shape == (3, 2))
        assert_array_equal(a, np.array([[1, 2], [3, 4], [5, 6]]))

    @pytest.mark.skipif(not _HAS_CTYPE,
                        reason="ctypes not available on this python installation")
    def test_pointer(self):
        from ctypes import c_int, cast, POINTER
        p = cast((c_int * 10)(*range(10)), POINTER(c_int))
        a = as_array(p, (10,))
        assert_(a.shape == (10,))
        assert_array_equal(a, np.array(range(10)))
