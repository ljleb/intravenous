# FFTW IV Package

This optional package provides the explicit node types:

- `iv.fftw.real_fft`, which transforms `Sample` values into an `FFTBlock`; and
- `iv.fftw.inverse_real_fft`, which transforms an `FFTBlock` into `Sample` values.

Core Intravenous contains neither operation and has no FFTW build or link dependency.
When the user selects this package, its CMake project downloads the official FFTW
consumer release into the package build directory and builds a package-local native
adapter. Only `backend.cpp` includes `fftw3.h`.

The package's LLVM node code remains hot-reloadable. The native adapter is retained for
the server process lifetime so every live node revision shares one FFTW planner lock;
restart the server after changing `backend.cpp` itself.

The current preliminary low-level nodes accept one transform size and therefore use a
hop equal to that transform size. The accepted final low-level interface accepts an
explicit transform size and hop size. Implementing a smaller hop also requires the
package to settle analysis/synthesis window ownership, overlap-add accumulation, and
discontinuous-timeline reset behavior; exposing a hop argument before those semantics
exist would misrepresent the implementation.

A future paired STFT module will accept transform size, hop size, inverse/window
configuration, and a user-supplied spectral `SubgraphClosure`, then pass the shared
configuration to the explicit forward and inverse nodes. That module waits for the
general `SubgraphClosure` construction-argument feature. Hop size is not inferred by
the generic port-constraint solver.

FFTW is licensed under the GNU General Public License. A user who redistributes an
FFTW-linked result is responsible for complying with the applicable FFTW/GPL terms.
The standalone Intravenous application and default builtin package do not contain or
link that result.
