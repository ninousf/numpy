"""
This file adds basic tests to test the NEP 50 style promotion compatibility
mode.  Most of these test are likely to be simply deleted again once NEP 50
is adopted in the main test suite.  A few may be moved elsewhere.
"""

import numpy as np
import pytest


@pytest.fixture(scope="module", autouse=True)
def _weak_promotion_enabled():
    state = np._get_promotion_state()
    np._set_promotion_state("weak_and_warn")
    yield
    np._set_promotion_state(state)


def test_nep50_examples():
    with pytest.warns(UserWarning, match="result dtype changed"):
        res = np.uint8(1) + 2
    assert res.dtype == np.uint8

    with pytest.warns(UserWarning, match="result dtype changed"):
        res = np.array([1], np.uint8) + np.int64(1)
    assert res.dtype == np.int64

    with pytest.warns(UserWarning, match="result dtype changed"):
        res = np.array([1], np.uint8) + np.array(1, dtype=np.int64)
    assert res.dtype == np.int64

    # Note: The following should warn due to overflow (depends gh-21437)
    with pytest.warns(UserWarning, match="result dtype changed"):
        res = np.float32(1) + 3e100
    assert np.isinf(res)

    # Changes, but we don't warn for it (too noisy)
    res = np.array([0.1], np.float32) == np.float64(0.1)
    assert res[0] == False

    # Additional test, since the above silences the warning:
    with pytest.warns(UserWarning, match="result dtype changed"):
        res = np.array([0.1], np.float32) + np.float64(0.1)
    assert res.dtype == np.float64

    with pytest.warns(UserWarning, match="result dtype changed"):
        res = np.array([1.], np.float32) + np.int64(3)
    assert res.dtype == np.float64


@pytest.mark.xfail
def test_nep50_integer_conversion_errors():
    # Implementation for error paths is mostly missing (as of writing)
    with pytest.raises(ValueError):  # (or TypeError?)
        np.array([1], np.uint8) + 300

    with pytest.raises(ValueError):  # (or TypeError?)
        np.uint8(1) + 300

