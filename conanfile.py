from conans import ConanFile, CMake, tools
import os

class VCVRackSDKConan(ConanFile):
    name = "VCVRackSDK"
    version = "latest"
    license = "GPL-3.0"
    author = "Andrew Belt"
    url = "https://github.com/VCVRack/Rack"
    homepage = "https://vcvrack.com"
    description = "VCVRackSDK - SDK for VCVRack plugin development."

    settings = "os", "compiler", "build_type", "arch"

    exports_sources = "*"

    generators = "cmake", "virtualrunenv"

    _lib_name = "rack"

    def requirements(self):
        self.requires.add("OUIBlendishVCVRack/latest@qno/testing")
        self.requires.add("OSDialog/latest@qno/testing")
        self.requires.add("NanoSVG/latest@qno/testing")
        self.requires.add("stb/20190512@conan/stable")

        self.requires.add("SpeexDSP/1.2rc3@qno/testing")
        self.requires.add("RtAudio/5.1.0@qno/testing")
        self.requires.add("RtMidi/4.0.0@qno/testing")
        self.requires.add("Pffft/latest@qno/testing")

        self.requires.add("jansson/2.12")
        self.requires.add("libcurl/7.64.1@bincrafters/stable")
        self.requires.add("libzip/1.5.1@bincrafters/stable")

        # override official glfw with VCVRack glfw version
        self.requires.add("glfw/3.3@qno/testing")
        #self.requires.add("glfw/3.3@bincrafters/stable")

        self.requires.add("libsamplerate/latest@qno/testing")

        #self.requires.add("Poco/1.9.0@pocoproject/stable")
        self.requires.add("nasm_installer/2.13.02@bincrafters/stable")

        # OpenSSL override
        self.requires.add("OpenSSL/1.1.1c@conan/stable")

    def build(self):
        if self.settings.os == "Windows":
            cmake = CMake(self)
            cmake.configure()
            cmake.build()

    def package(self):
        self.copy("*.*", dst="include", src="include")
        self.copy("add_binary.cmake")

        if self.settings.os == "Windows":
            self.copy("*Rack*.lib", dst="lib", keep_path=False)
            self.copy("*Rack*.a", dst="lib", keep_path=False)

    def package_info(self):

        if self.settings.os == "Windows":
            self.cpp_info.cppflags.append("-DARCH_WIN")
            self.cpp_info.libs.append(self._lib_name)

        if self.settings.os == "Linux":
            self.cpp_info.cppflags.append("-DARCH_LIN")

        if self.settings.os == "Macos":
            self.cpp_info.cppflags.append("-DARCH_MAC")

        if not self._isVisualStudioBuild():
            self.cpp_info.cppflags.append("-march=nocona -funsafe-math-optimizations")

    def deploy(self):
        self.copy("Rack*", dst="dist/Rack", src="bin", keep_path=False)
        self.copy("res", dst="dist/Rack", keep_path=False)
        self.copy("LICENSE*", dst="dist/Rack", keep_path=False)
        self.copy("template.vcv", dst="dist/Rack", keep_path=False)
        self.copy("Core.json", dst="dist/Rack", keep_path=False)

        if self._isMinGWBuild():
            self.copy_deps("libwinpthread*.dll", dst="dist/Rack", keep_path=False)
            self.copy_deps("libstdc++*.dll", dst="dist/Rack", keep_path=False)
            self.copy_deps("libgcc_s_seh*.dll", dst="dist/Rack", keep_path=False)

    def _isVisualStudioBuild(self):
        return self.settings.os == "Windows" and self.settings.compiler == "Visual Studio"

    def _isMinGWBuild(self):
        return self.settings.os == "Windows" and self.settings.compiler == "gcc"
