"""Build C extension for Pidgin acceleration."""

from setuptools import setup, Extension

setup(
    ext_modules=[
        Extension(
            "pidgin._fast",
            sources=["src/pidgin/_fast.c"],
        ),
    ],
)
