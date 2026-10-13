# End-to-end test of romtool on synthetic fixtures.
file(REMOVE_RECURSE ${WORKDIR})
file(MAKE_DIRECTORY ${WORKDIR})

function(run expect_rc out_var)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL expect_rc)
    message(FATAL_ERROR "expected rc=${expect_rc} got ${rc} for: ${ARGN}\n${out}\n${err}")
  endif()
  set(${out_var} "${out}${err}" PARENT_SCOPE)
endfunction()

run(0 o ${MKFIXTURE} ${WORKDIR})
run(0 info ${ROMTOOL} info ${WORKDIR}/test.cci)
if(NOT info MATCHES "fs:USER")
  message(FATAL_ERROR "info missing services: ${info}")
endif()

# profile -> extract with profile succeeds; wrong profile fails
run(0 prof ${ROMTOOL} profile ${WORKDIR}/test.cci)
file(WRITE ${WORKDIR}/good.profile "${prof}")
run(0 o ${ROMTOOL} extract ${WORKDIR}/test.cci ${WORKDIR}/out --profile ${WORKDIR}/good.profile)
if(NOT EXISTS ${WORKDIR}/out/exefs/code.bin)
  message(FATAL_ERROR "code.bin not written")
endif()
file(SIZE ${WORKDIR}/out/exefs/code.bin sz)
if(NOT sz EQUAL 9000)
  message(FATAL_ERROR "code.bin size ${sz}")
endif()
string(REGEX REPLACE "code_sha256 = [0-9a-f]+" "code_sha256 = 0000000000000000000000000000000000000000000000000000000000000000" bad "${prof}")
file(WRITE ${WORKDIR}/bad.profile "${bad}")
run(1 o ${ROMTOOL} extract ${WORKDIR}/test.cci ${WORKDIR}/out2 --profile ${WORKDIR}/bad.profile)
if(NOT o MATCHES "unsupported build")
  message(FATAL_ERROR "expected unsupported build message: ${o}")
endif()
# no profile and no --allow-unknown -> refuse
run(1 o ${ROMTOOL} extract ${WORKDIR}/test.cci ${WORKDIR}/out3)
run(0 o ${ROMTOOL} extract ${WORKDIR}/test.cci ${WORKDIR}/out3 --allow-unknown)

# update overlay changes code and fails the base profile
run(1 o ${ROMTOOL} extract ${WORKDIR}/test.cci ${WORKDIR}/out4 --profile ${WORKDIR}/good.profile --update ${WORKDIR}/update.cxi)
run(0 o ${ROMTOOL} extract ${WORKDIR}/test.cci ${WORKDIR}/out4 --allow-unknown --update ${WORKDIR}/update.cxi)
file(SIZE ${WORKDIR}/out4/exefs/code.bin sz)
if(NOT sz EQUAL 9001)
  message(FATAL_ERROR "update code size ${sz}")
endif()

run(0 ls ${ROMTOOL} ls ${WORKDIR}/test.cci)
if(NOT ls MATCHES "/d/x.bin")
  message(FATAL_ERROR "ls: ${ls}")
endif()
run(0 cat ${ROMTOOL} cat ${WORKDIR}/test.cci /hello.txt)
if(NOT cat STREQUAL "hello")
  message(FATAL_ERROR "cat: ${cat}")
endif()

# encrypted input is refused with a clear message
run(1 o ${ROMTOOL} info ${WORKDIR}/encrypted.cxi)
if(NOT o MATCHES "decrypted")
  message(FATAL_ERROR "encrypted message: ${o}")
endif()
# missing file
run(1 o ${ROMTOOL} info ${WORKDIR}/nope.cci)
message(STATUS "romtool CLI OK")
