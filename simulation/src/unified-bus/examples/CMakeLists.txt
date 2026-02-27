build_lib_example(
  NAME ub-quick-example
  SOURCE_FILES ub-quick-example.cc
  LIBRARIES_TO_LINK ${libcore}
                    ${libconfig-store}
                    ${libpoint-to-point}
                    ${libmtp}
)
