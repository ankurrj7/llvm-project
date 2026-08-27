=====================================
LLVM text coverage format 4: segments
=====================================

.. program:: llvm-cov

Overview
========

Text coverage format 4 is a tab-separated, streaming coverage format produced
by ``llvm-cov export --txtcvrg-view=segments``. It is intended for consumers
that need stable function identities and ordered, non-overlapping executable
source intervals. It is not a replacement for the annotated source produced by
``llvm-cov show``.

Use ``--txtcvrgfull`` to create a complete, all-zero structural baseline::

  llvm-cov export coverage.spi --txtcvrgfull \
    --txtcvrg-view=segments > baseline.txt

Use ``--txtcvrg`` with an instrumentation profile to create an execution
report containing hit functions::

  llvm-cov export coverage.spi --instr-profile=run.profdata \
    --txtcvrg --txtcvrg-view=segments > execution.txt

The mapping input may also be a covered object or executable accepted by
``llvm-cov export``. Baseline generation does not require a profile.

Format grammar
==============

Every field is separated by one tab character. Locations use one-based
``line.column`` coordinates. Quoted strings use LLVM escaped-string syntax.

.. code-block:: text

  txtcvrg\t4\t<baseline|execution>\tview=segments\
  \tbranches=<0|1>\tmcdc=<0|1>

  file\t"<root-source-file>"
  function\t"<display-name>"\t<total>\t<hit>\t<percent>
  1\t<start>\t<end>\t<count>
  2\t<start>\t<end>\t<count>
  ...

  file\t"<macro-body-source-file>"
  1\t<start>\t<end>\t<count>
  2\t<start>\t<end>\t<count>
  ...
  end

The first record identifies format version 4, the export mode, the selected
view, and whether optional branch or MC/DC records are present. Blank lines may
separate file groups. The final record is always ``end``.

Files and functions
===================

A ``file`` record establishes the source file for following function records.
The same filename may occur again later. Consumers must therefore treat each
``file`` record as a new block instead of requiring filenames to be globally
unique.

A ``function`` record contains:

* the display name;
* the number of kind ``1`` root-file records;
* the number of those records whose count is nonzero; and
* ``100 * hit / total``, printed with two decimal places.

Raw profile names, function hashes, and entry counts are not part of the
format. Functions with the same display name remain separate records and
should be identified together with their file and occurrence order.

Numeric records
===============

Kind ``1`` is executable coverage. Positive-width kind ``1`` records are
ordered by source location and do not overlap within a function. Nested raw
regions are split at structural boundaries, and the innermost executable
region supplies the count. Gap and skipped regions mask executable coverage.

Kind ``2`` describes a macro or include expansion site. It is metadata about
the expansion and does not represent an additional executable counter. Kind
``2`` records do not contribute to function totals or percentages.

Macro-body coverage owned by another physical source file is emitted after a
``file`` record without a following ``function`` record. Positive-width kind
``1`` intervals in each such source block are also normalized so they do not
overlap. Their counts may aggregate executions from multiple expansion sites.

Function counter points
=======================

A macro-generated function can have coverage counters but no positive-width
executable interval after gap and macro normalization. Removing that function
would also remove the only evidence that it ran. Format 4 therefore emits one
kind ``1`` point record for each of its raw code counters:

.. code-block:: text

  function\t"generated_wrapper"\t1\t0\t0.00
  1\t385.1\t385.1\t0
  2\t385.1\t385.29\t0

For a point record, start and end are equal. The location is the original code
region's start. A point is a counter marker, not a source interval, so it does
not overlap source text. Its identity is stable between baseline and execution
exports, and its count determines the function's hit state like any other kind
``1`` record. Multiple point records may share a location when the mapping has
multiple distinct code counters there.

Baseline and execution invariants
=================================

Normalization uses mapping structure and static counter identity, never
evaluated runtime counts. Given the same mapping input:

* ``--txtcvrgfull`` emits every reportable function with zero counts;
* ``--txtcvrg`` omits functions for which every kind ``1`` count is zero;
* a function present in both reports has the same kind, start, and end records;
* only counts and the function hit summary change; and
* zero-count intervals inside a selected hit function remain present.

These rules allow a consumer to build structural identities from the baseline
and apply execution counts without reconstructing LLVM's overlapping raw
coverage regions.

Optional records
================

``--include-branches`` adds records of this form:

.. code-block:: text

  branch\t"<source-file>"\t<start>\t<end>\t<true-count>\
  \t<false-count>\t<true-folded>\t<false-folded>

``--include-mcdc`` adds MC/DC branch and decision records:

.. code-block:: text

  mcdc-branch\t"<source-file>"\t<start>\t<end>\t<true-count>\
  \t<false-count>\t<condition-id>\t<true-next-id>\t<false-next-id>\
  \t<true-folded>\t<false-folded>
  mcdc-decision\t"<source-file>"\t<start>\t<end>\t<true-decisions>\
  \t<false-decisions>\t<conditions>\t<covered-conditions>\
  \t<folded-conditions>

These records carry their own source filename and do not contribute to the
kind ``1`` function totals.

Consumer guidance
=================

For source annotation, process each function's positive-width kind ``1``
records in order and mark the half-open interval ``[start, end)`` hit when its
count is nonzero. Treat equal-start/end records only as function counter
points. Preserve kind ``2`` separately if expansion-site reporting is needed.

Do not compare format 3 region percentages directly with format 4 segment
percentages. Format 3 counts overlapping raw code regions; format 4 counts
normalized segments or fallback counter points. The hit meaning is preserved,
but the denominator is different.

Compatibility
=============

``--txtcvrg-view=regions`` remains format 3 and retains the original overlapping
region layout. Consumers must select format 4 explicitly and reject unknown
version numbers or view fields rather than silently interpreting them as
format 3.
