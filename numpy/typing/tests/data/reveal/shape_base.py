import numpy as np
from numpy.typing import NDArray
from typing import Any, List

i8: np.int64
f8: np.float64

AR_b: NDArray[np.bool_]
AR_i8: NDArray[np.int64]
AR_f8: NDArray[np.float64]

AR_LIKE_f8: List[float]

reveal_type(np.take_along_axis(AR_f8, AR_i8, axis=1))  # E: numpy.ndarray[Any, numpy.dtype[{float64}]]
reveal_type(np.take_along_axis(f8, AR_i8, axis=None))  # E: numpy.ndarray[Any, numpy.dtype[{float64}]]

reveal_type(np.put_along_axis(AR_f8, AR_i8, "1.0", axis=1))  # E: None

reveal_type(np.expand_dims(AR_i8, 2))  # E: numpy.ndarray[Any, numpy.dtype[{int64}]]
reveal_type(np.expand_dims(AR_LIKE_f8, 2))  # E: numpy.ndarray[Any, numpy.dtype[Any]]

reveal_type(np.column_stack([AR_i8]))  # E: numpy.ndarray[Any, numpy.dtype[{int64}]]
reveal_type(np.column_stack([AR_LIKE_f8]))  # E: numpy.ndarray[Any, numpy.dtype[Any]]

reveal_type(np.dstack([AR_i8]))  # E: numpy.ndarray[Any, numpy.dtype[{int64}]]
reveal_type(np.dstack([AR_LIKE_f8]))  # E: numpy.ndarray[Any, numpy.dtype[Any]]

reveal_type(np.row_stack([AR_i8]))  # E: numpy.ndarray[Any, numpy.dtype[{int64}]]
reveal_type(np.row_stack([AR_LIKE_f8]))  # E: numpy.ndarray[Any, numpy.dtype[Any]]

reveal_type(np.array_split(AR_i8, [3, 5, 6, 10]))  # E: list[numpy.ndarray[Any, numpy.dtype[{int64}]]]
reveal_type(np.array_split(AR_LIKE_f8, [3, 5, 6, 10]))  # E: list[numpy.ndarray[Any, numpy.dtype[Any]]]

reveal_type(np.split(AR_i8, [3, 5, 6, 10]))  # E: list[numpy.ndarray[Any, numpy.dtype[{int64}]]]
reveal_type(np.split(AR_LIKE_f8, [3, 5, 6, 10]))  # E: list[numpy.ndarray[Any, numpy.dtype[Any]]]

reveal_type(np.hsplit(AR_i8, [3, 5, 6, 10]))  # E: list[numpy.ndarray[Any, numpy.dtype[{int64}]]]
reveal_type(np.hsplit(AR_LIKE_f8, [3, 5, 6, 10]))  # E: list[numpy.ndarray[Any, numpy.dtype[Any]]]

reveal_type(np.vsplit(AR_i8, [3, 5, 6, 10]))  # E: list[numpy.ndarray[Any, numpy.dtype[{int64}]]]
reveal_type(np.vsplit(AR_LIKE_f8, [3, 5, 6, 10]))  # E: list[numpy.ndarray[Any, numpy.dtype[Any]]]

reveal_type(np.dsplit(AR_i8, [3, 5, 6, 10]))  # E: list[numpy.ndarray[Any, numpy.dtype[{int64}]]]
reveal_type(np.dsplit(AR_LIKE_f8, [3, 5, 6, 10]))  # E: list[numpy.ndarray[Any, numpy.dtype[Any]]]

reveal_type(np.lib.shape_base.get_array_prepare(AR_i8))  # E: numpy.lib.shape_base._ArrayPrepare
reveal_type(np.lib.shape_base.get_array_prepare(AR_i8, 1))  # E: Union[None, numpy.lib.shape_base._ArrayPrepare]

reveal_type(np.get_array_wrap(AR_i8))  # E: numpy.lib.shape_base._ArrayWrap
reveal_type(np.get_array_wrap(AR_i8, 1))  # E: Union[None, numpy.lib.shape_base._ArrayWrap]

reveal_type(np.kron(AR_b, AR_b))  # E: numpy.ndarray[Any, numpy.dtype[numpy.bool_]]
reveal_type(np.kron(AR_b, AR_i8))  # E: numpy.ndarray[Any, numpy.dtype[numpy.signedinteger[Any]]]
reveal_type(np.kron(AR_f8, AR_f8))  # E: numpy.ndarray[Any, numpy.dtype[numpy.floating[Any]]]

reveal_type(np.tile(AR_i8, 5))  # E: numpy.ndarray[Any, numpy.dtype[{int64}]]
reveal_type(np.tile(AR_LIKE_f8, [2, 2]))  # E: numpy.ndarray[Any, numpy.dtype[Any]]
