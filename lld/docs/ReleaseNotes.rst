===========================
lld |release| Release Notes
===========================

.. contents::
    :local:

.. only:: PreRelease

  .. warning::
     These are in-progress notes for the upcoming LLVM |release| release.
     Release notes for previous releases can be found on
     `the Download Page <https://releases.llvm.org/download.html>`_.

Introduction
============

This document contains the release notes for the lld linker, release |release|.
Here we describe the status of lld, including major improvements
from the previous release. All lld releases may be downloaded
from the `LLVM releases web site <https://llvm.org/releases/>`_.

Non-comprehensive list of changes in this release
=================================================

ELF Improvements
----------------
* For AArch64, added support for ``-zgcs-report-dynamic``, enabling checks for
  GNU GCS Attribute Flags in Dynamic Objects when GCS is enabled. Inherits value
  from ``-zgcs-report`` (capped at ``warning`` level) unless user-defined,
  ensuring compatibility with GNU ld linker.

* The default Hexagon architecture version in ELF object files produced by
  lld is changed to v68. This change is only effective when the version is
  not provided in the command line by the user and cannot be inferred from
  inputs.

* ``--why-live=<glob>`` prints for each symbol matching ``<glob>`` a chain of
  items that kept it live during garbage collection. This is inspired by the
  Mach-O LLD feature of the same name.

* Linker script ``OVERLAY`` descriptions now support virtual memory regions
  (e.g. ``>region``) and ``NOCROSSREFS``.

Breaking changes
----------------

COFF Improvements
-----------------

MinGW Improvements
------------------

MachO Improvements
------------------

WebAssembly Improvements
------------------------

Fixes
#####
