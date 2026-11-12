#!/usr/bin/env python3
r"""Writes the observations of the example workflow to standard output, as a long-form CSV.

Two series of one variable, ``population`` (unit ``count``), observed at irregular times within
[0, 12) days:

- S01 follows the logistic curve with x0 = 10, r = 0.30 and K = 100;
- S02 follows the one with x0 = 25, r = 0.25 and K = 80.

Each series has 15 rows, 30 in all, sorted by time (then series). The value of row ``i`` (counted
from 0 in that order) is the curve's value multiplied by ``1 + 0.03 * sin(1.7 * i)`` and written
with three decimals. One row is missing (``missing=true`` and an empty value), and one is
left-censored: an empty value below its ``upper_limit``.

The output is deterministic and the script uses only the Python standard library. To regenerate
the committed file, from the repository root::

    python3 tools/generate_example_data.py \
        > examples/workflows/estimate-predict/data/observations.csv
"""

import math
import sys

ROWS_PER_SERIES = 15

# series_id, x0, r, K, the first time (in hundredths of a day), the residue that shifts its times
SERIES = (
    ("S01", 10.0, 0.30, 100.0, 0, 0),
    ("S02", 25.0, 0.25, 80.0, 20, 2),
)

# (series_id, k): the k-th row of the series is missing.
MISSING = ("S02", 6)

# (series_id, k, upper_limit): the k-th row of the series is left-censored.
LEFT_CENSORED = ("S01", 10, "60.0")

HEADER = "observation_id,series_id,variable_id,time,value,unit,missing,censoring,upper_limit"


def logistic(x0, r, capacity, t):
    """The logistic curve x(t) = K / (1 + (K / x0 - 1) * exp(-r t))."""
    return capacity / (1.0 + (capacity / x0 - 1.0) * math.exp(-r * t))


def hundredths(first, residue):
    """Irregular times in hundredths of a day: 0.8 days apart, each one shifted by 0, 0.17, 0.34,
    0.51 or 0.68 days, as (k^2 + 3k + residue) mod 5 says. Whole numbers, so no rounding."""
    return [first + 80 * k + 17 * ((k * k + 3 * k + residue) % 5) for k in range(ROWS_PER_SERIES)]


def decimal_time(time):
    """A time in hundredths of a day as exact decimal text, for example 54 -> "0.54"."""
    return "%d.%02d" % (time // 100, time % 100)


def rows():
    """The rows (time in hundredths, series_id, k, curve value), sorted by time, then series."""
    result = []
    for series, x0, r, capacity, first, residue in SERIES:
        for k, time in enumerate(hundredths(first, residue)):
            if time >= 1200:
                raise ValueError("time %s of series %s is not within [0, 12)" % (time, series))
            result.append((time, series, k, logistic(x0, r, capacity, time / 100.0)))
    result.sort(key=lambda row: (row[0], row[1]))
    return result


def csv_lines():
    lines = [HEADER]
    for i, (time, series, k, value) in enumerate(rows()):
        text = "%.3f" % (value * (1.0 + 0.03 * math.sin(1.7 * i)))
        missing = "false"
        censoring = "none"
        upper_limit = ""
        if (series, k) == MISSING:
            text = ""
            missing = "true"
        elif (series, k) == LEFT_CENSORED[:2]:
            text = ""
            censoring = "left"
            upper_limit = LEFT_CENSORED[2]
        lines.append(",".join(["%s-%02d" % (series, k), series, "population", decimal_time(time),
                               text, "count", missing, censoring, upper_limit]))
    return lines


def main():
    # Bytes, so the line endings are "\n" on every platform.
    sys.stdout.buffer.write(("\n".join(csv_lines()) + "\n").encode("ascii"))


if __name__ == "__main__":
    main()
