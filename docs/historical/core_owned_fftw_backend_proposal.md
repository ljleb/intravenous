# Superseded Core-Owned FFTW Backend Proposal

_Provenance: preserved on 2026-10-16 before the accepted FFTW IV-package
boundary replaced this preliminary implementation direction. This document is
historical and non-normative. The current direction is recorded in
[Port Realization Migration Status](../port_realization_migration_status.md) and
[Random Access Port Data Direction](../random_access_port_data_direction.md)._

The earlier implementation-status text said:

> The first executable FFT realization may use FFTW's single-precision real
> transforms. The canonical `FFTBlock` representation remains independent of
> FFTW: an even transform of length `N` stores `N / 2 + 1` explicit
> `std::complex<float>` bins, including DC and Nyquist. This matches FFTW's
> logical out-of-place real-transform result, but FFTW plan objects, alignment
> rules, destructive-input behavior, normalization, and scratch storage remain
> private implementation details of the FFT node realization.
>
> FFTW plan creation and destruction occur off the realtime thread. Realtime
> callbacks may execute only already-created plans against storage whose
> alignment and layout were accepted by realization. A later project-owned FFT
> kernel must be able to replace this backend without changing port declarations,
> `FFTBlock`, configured graphs, or node source.
>
> This backend choice is preliminary because FFTW's GPL terms are operationally
> inconvenient. Local and personal builds may fetch and link FFTW. A distributed
> build that links FFTW must satisfy the GPL obligations for that combined build,
> including the applicable license and corresponding-source requirements;
> copying a license file alone is not the complete obligation. Release packaging
> must therefore either enable the GPL FFTW backend deliberately or use a future
> project-owned/permissively licensed kernel. This is implementation and
> release-policy status, not a requirement that the final application depend on
> FFTW.

The earlier normative interoperability text also assigned native FFT adapter
ownership to the continuous-value extension:

> FFT-library adapters owned by the continuous-value extension may
> consume/produce a library's native packed form or convert it at the boundary.

That ownership was rejected. A continuous-value extension owns only the canonical
graph value, storage geometry, and generic value operations. It must not cause the
application, GraphJit, or `iv_builder` to depend on a transform implementation.
Explicit transform nodes own their numerical transform and any native adapter.

The abandoned source implementation linked FFTW into `iv_builder` and resolved
hot-loaded FFT nodes through that process-wide library. That made every built
application carry FFTW whether or not any graph selected an FFT node. The accepted
design instead makes FFTW an independently selected IV package dependency.
