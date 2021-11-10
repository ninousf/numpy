"""
Pytest configuration and fixtures for the Numpy test suite.
"""
import pytest
import numpy

@pytest.fixture(autouse=True)
def add_np(doctest_namespace):
    numpy.random.seed(1)
    doctest_namespace['np'] = numpy

pytest_plugins = ["pytest_doctestplus"]
