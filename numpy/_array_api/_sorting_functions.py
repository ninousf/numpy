from __future__ import annotations

from ._types import array
from ._array_object import ndarray

import numpy as np

def argsort(x: array, /, *, axis: int = -1, descending: bool = False, stable: bool = True) -> array:
    """
    Array API compatible wrapper for :py:func:`np.argsort <numpy.argsort>`.

    See its docstring for more information.
    """
    # Note: this keyword argument is different, and the default is different.
    kind = 'stable' if stable else 'quicksort'
    res = np.argsort(x._array, axis=axis, kind=kind)
    if descending:
        res = np.flip(res, axis=axis)
    return ndarray._new(res)

def sort(x: array, /, *, axis: int = -1, descending: bool = False, stable: bool = True) -> array:
    """
    Array API compatible wrapper for :py:func:`np.sort <numpy.sort>`.

    See its docstring for more information.
    """
    # Note: this keyword argument is different, and the default is different.
    kind = 'stable' if stable else 'quicksort'
    res = np.sort(x._array, axis=axis, kind=kind)
    if descending:
        res = np.flip(res, axis=axis)
    return ndarray._new(res)
