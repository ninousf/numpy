from .mtrand import RandomState
from .philox import Philox
from .pcg64 import PCG64
from .xoshiro256 import Xoshiro256
from .xoshiro512 import Xoshiro512

from .generator import Generator
from .mt19937 import MT19937

BitGenerators = {'MT19937': MT19937,
                 'PCG64': PCG64,
                 'Philox': Philox,
                 'Xoshiro256': Xoshiro256,
                 'Xoshiro512': Xoshiro512
                 }


def __generator_ctor(bit_generator_name='mt19937'):
    """
    Pickling helper function that returns a Generator object

    Parameters
    ----------
    bit_generator_name: str
        String containing the core BitGenerator

    Returns
    -------
    rg: Generator
        Generator using the named core BitGenerator
    """
    if bit_generator_name in BitGenerators:
        bit_generator = BitGenerators[bit_generator_name]
    else:
        raise ValueError(str(bit_generator_name) + ' is not a known '
                                                   'BitGenerator module.')

    return Generator(bit_generator())


def __bit_generator_ctor(bit_generator_name='mt19937'):
    """
    Pickling helper function that returns a bit generator object

    Parameters
    ----------
    bit_generator_name: str
        String containing the name of the BitGenerator

    Returns
    -------
    bit_generator: BitGenerator
        BitGenerator instance
    """
    if bit_generator_name in BitGenerators:
        bit_generator = BitGenerators[bit_generator_name]
    else:
        raise ValueError(str(bit_generator_name) + ' is not a known '
                                                   'BitGenerator module.')

    return bit_generator()


def __randomstate_ctor(bit_generator_name='mt19937'):
    """
    Pickling helper function that returns a legacy RandomState-like object

    Parameters
    ----------
    bit_generator_name: str
        String containing the core BitGenerator

    Returns
    -------
    rs: RandomState
        Legacy RandomState using the named core BitGenerator
    """
    if bit_generator_name in BitGenerators:
        bit_generator = BitGenerators[bit_generator_name]
    else:
        raise ValueError(str(bit_generator_name) + ' is not a known '
                                                   'BitGenerator module.')

    return RandomState(bit_generator())
