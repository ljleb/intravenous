# Beats constructor origin

_Historical. Preserved 2026-11-18 from the builtin usage guide and the Vue
implementation checkpoint. The owner chose to anchor Beats at sample zero and
keep any time shift outside the generator. See the [current builtin usage
guide](../../src/intravenous/builtin_packages/README.md#beats)._

The original `Beats` constructor took `origin_sample`, a signed sample index
for the bar line at the time origin. It defaulted to `0`. The node used that
sample as its phase anchor when computing the marks in each requested window.
The initial Beats presentation showed the accepted origin along with the other
construction arguments.
