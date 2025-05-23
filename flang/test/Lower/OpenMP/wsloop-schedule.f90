! This test checks lowering of OpenMP DO Directive(Worksharing) with
! simd schedule modifier.

! RUN: bbc -fopenmp -emit-hlfir %s -o - | FileCheck %s

program wsloop_dynamic
  integer :: i
!CHECK-LABEL: func @_QQmain()

!$OMP PARALLEL
!CHECK:  omp.parallel {

!$OMP DO SCHEDULE(simd: runtime)
!CHECK:      %[[WS_LB:.*]] = arith.constant 1 : i32
!CHECK:      %[[WS_UB:.*]] = arith.constant 9 : i32
!CHECK:      %[[WS_STEP:.*]] = arith.constant 1 : i32
!CHECK:      omp.wsloop nowait schedule(runtime, simd) private({{.*}}) {
!CHECK-NEXT:   omp.loop_nest (%[[I:.*]]) : i32 = (%[[WS_LB]]) to (%[[WS_UB]]) inclusive step (%[[WS_STEP]]) {
!CHECK:          hlfir.assign %[[I]] to %[[STORE:.*]]#0 : i32, !fir.ref<i32>

  do i=1, 9
    print*, i
!CHECK:          %[[RTBEGIN:.*]] = fir.call @_FortranAioBeginExternalListOutput
!CHECK:          %[[LOAD:.*]] = fir.load %[[STORE]]#0 : !fir.ref<i32>
!CHECK:          fir.call @_FortranAioOutputInteger32(%[[RTBEGIN]], %[[LOAD]]) {{.*}}: (!fir.ref<i8>, i32) -> i1
!CHECK:          fir.call @_FortranAioEndIoStatement(%[[RTBEGIN]]) {{.*}}: (!fir.ref<i8>) -> i32
  end do
!CHECK:          omp.yield
!CHECK:        }
!CHECK:      }
!CHECK:      omp.terminator
!CHECK:    }

!$OMP END DO NOWAIT
!$OMP END PARALLEL
end
