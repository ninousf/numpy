"""
Various richly-typed exceptions, that also help us deal with string formatting
in python where it's easier.

By putting the formatting in `__str__`, we also avoid paying the cost for
users who silence the exceptions.
"""

def _unpack_tuple(tup):
    if len(tup) == 1:
        return tup[0]
    else:
        return tup


def _display_as_base(cls):
    """
    A decorator that makes an exception class look like its base.

    We use this to hide subclasses that are implementation details - the user
    should catch the base type, which is what the traceback will show them.

    Classes decorated with this decorator are subject to removal without a
    deprecation warning.
    """
    assert issubclass(cls, Exception)
    cls.__name__ = cls.__base__.__name__
    cls.__qualname__ = cls.__base__.__qualname__
    return cls


class UFuncTypeError(TypeError):
    """ Base class for all ufunc exceptions """
    def __init__(self, ufunc):
        self.ufunc = ufunc


@_display_as_base
class _UFuncNoLoopError(UFuncTypeError):
    """ Thrown when a ufunc loop cannot be found """
    def __init__(self, ufunc, dtypes):
        super().__init__(ufunc)
        self.dtypes = tuple(dtypes)

    def __str__(self):
        return (
            "ufunc {!r} did not contain a loop with signature matching types "
            "{!r} -> {!r}"
        ).format(
            self.ufunc.__name__,
            _unpack_tuple(self.dtypes[:self.ufunc.nin]),
            _unpack_tuple(self.dtypes[self.ufunc.nin:])
        )


@_display_as_base
class _UFuncCastingError(UFuncTypeError):
    def __init__(self, ufunc, casting, from_, to):
        super().__init__(ufunc)
        self.casting = casting
        self.from_ = from_
        self.to = to


@_display_as_base
class _UFuncInputCastingError(_UFuncCastingError):
    """ Thrown when a ufunc input cannot be casted """
    def __init__(self, ufunc, casting, from_, to, i):
        super().__init__(ufunc, casting, from_, to)
        self.in_i = i

    def __str__(self):
        # only show the number if more than one input exists
        i_str = "{} ".format(self.in_i) if self.ufunc.nin != 1 else ""
        return (
            "Cannot cast ufunc {!r} input {}from {!r} to {!r} with casting "
            "rule {!r}"
        ).format(
            self.ufunc.__name__, i_str, self.from_, self.to, self.casting
        )


@_display_as_base
class _UFuncOutputCastingError(_UFuncCastingError):
    """ Thrown when a ufunc output cannot be casted """
    def __init__(self, ufunc, casting, from_, to, i):
        super().__init__(ufunc, casting, from_, to)
        self.out_i = i

    def __str__(self):
        # only show the number if more than one output exists
        i_str = "{} ".format(self.out_i) if self.ufunc.nout != 1 else ""
        return (
            "Cannot cast ufunc {!r} output {}from {!r} to {!r} with casting "
            "rule {!r}"
        ).format(
            self.ufunc.__name__, i_str, self.from_, self.to, self.casting
        )
