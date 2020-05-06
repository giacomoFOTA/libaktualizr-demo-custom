# libaktualizr-demo-custom
This is a custom branch of libaktualizr-demo-app (credits to advancedtelematic) modified to suit my needs. 
In particular: 
- there is a specific command to flash firmware to an Arduino secondary ECU (to be used as a proof of concept)
 

### Note: libaktualizr-demo-app must be built "together" with meta-updater, i.e. having the same aktualizr version, otherwise it won't work properly

## Installation procedure
Including this layer in the project directly do not work (bitbake fails when building gRPC). The solution we found consists in modifying slightly aktualizr, so that gRPC is not build but aktualizr version is overall almost the same as the one specified by /meta-updater/recipes-sota/aktualizr_git.bb (in this file you have to force the same branch and SRCREV you see here, i.e. feat/grpc-hmi SRCREV="be145b6ac5021d2ceb349b29be53c45d0bc3b7d3").
So, to have successful compiling, you have to start bitbake a first time, then open /build/tmp/work/.../libaktualizr-demo/.../git/aktualizr and modify some files:
- /src/CMakeList.txt: delete row add_subdirectory("grpc")
- CMakeList.txt: delete the whole section starting at line 422 (# Check if some source files were not added sent to `aktualizr_source_file_checks)

Additional modification to avoid errors in general:
- CMakeList.txt: delete the section starting at line 100 (if(NOT AKTUALIZR_VERSION)...) and remove also line 199, 200, 201 (if (WARNING_AS_ERROR)...)

