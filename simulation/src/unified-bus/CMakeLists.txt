set(mpi_sources)
set(mpi_headers)
set(mpi_libraries)

if(${ENABLE_MPI})
  set(mpi_sources
      # model/point-to-point-remote-channel.cc
  )
  set(mpi_headers
      # model/point-to-point-remote-channel.h
  )
  set(mpi_libraries
      # ${libmpi}
      # ${MPI_CXX_LIBRARIES}
  )
endif()

build_lib(
  LIBNAME unified-bus
  SOURCE_FILES
    ${mpi_sources}
	model/ub-traffic-gen.cc
	model/ub-app.cc
    model/ub-controller.cc
	model/protocol/ub-datalink.cc
	model/ub-datatype.cc
	model/protocol/ub-header.cc
	model/ub-link.cc
	model/ub-port.cc
	model/ub-switch.cc
	model/protocol/ub-transaction.cc
	model/protocol/ub-function.cc
	model/protocol/ub-transport.cc
	model/protocol/ub-routing-process.cc
	model/ub-switch-allocator.cc
	model/ub-utils.cc
	model/protocol/ub-ldst-api.cc
	model/ub-ldst-thread.cc
	model/ub-ldst-instance.cc
	model/protocol/ub-congestion-control.cc
	model/protocol/ub-caqm.cc
	model/protocol/ub-flow-control.cc
	model/ub-queue-manager.cc
	model/ub-fault.cc
	model/ub-tp-connection-manager.cc
  HEADER_FILES
    ${mpi_headers}
	model/ub-traffic-gen.h
	model/ub-app.h
    model/ub-controller.h
	model/protocol/ub-datalink.h
	model/ub-datatype.h
	model/protocol/ub-header.h
	model/ub-link.h
	model/ub-port.h
	model/ub-switch.h
	model/protocol/ub-transaction.h
	model/protocol/ub-function.h
	model/protocol/ub-transport.h
	model/protocol/ub-routing-process.h
	model/ub-switch-allocator.h
	model/ub-utils.h
	model/ub-network-address.h
	model/ub-tp-connection-manager.h
	model/protocol/ub-ldst-api.h
	model/ub-ldst-thread.h
	model/ub-ldst-instance.h
	model/protocol/ub-congestion-control.h
	model/protocol/ub-caqm.h
	model/protocol/ub-flow-control.h
	model/ub-queue-manager.h
	model/ub-tag.h
	model/ub-fault.h
  LIBRARIES_TO_LINK ${libnetwork}
                    ${libinternet}
                    ${mpi_libraries}
  TEST_SOURCES test/ub-test.cc
)

