from __future__ import annotations

from ._array_object import ndarray

from typing import TYPE_CHECKING
if TYPE_CHECKING:
    from ._types import Optional, Tuple, Union, Array

import numpy as np

def all(x: Array, /, *, axis: Optional[Union[int, Tuple[int, ...]]] = None, keepdims: bool = False) -> Array:
    """
    Array API compatible wrapper for :py:func:`np.all <numpy.all>`.

    See its docstring for more information.
    """
    return ndarray._new(np.asarray(np.all(x._array, axis=axis, keepdims=keepdims)))

def any(x: Array, /, *, axis: Optional[Union[int, Tuple[int, ...]]] = None, keepdims: bool = False) -> Array:
    """
    Array API compatible wrapper for :py:func:`np.any <numpy.any>`.

    See its docstring for more information.
    """
    return ndarray._new(np.asarray(np.any(x._array, axis=axis, keepdims=keepdims)))
