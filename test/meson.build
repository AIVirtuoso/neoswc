# Unit tests for internal libswc modules. These compile the module sources in
# directly rather than linking libswc: the library is built with hidden
# visibility, and these modules are deliberately free of libswc dependencies so
# they can be exercised without a display.

transaction_test = executable('transaction_test',
  ['transaction_test.c'] + swc_transaction_src,
  include_directories: swc_transaction_inc,
  dependencies: [dependency('wayland-server')],
)
test('transaction', transaction_test)
