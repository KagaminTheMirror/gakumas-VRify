from conan import ConanFile
from conan.tools.cmake import CMakeDeps, CMakeToolchain


class VrDependencies(ConanFile):
    settings = "os", "arch", "compiler", "build_type"
    requires = (
        "cpprestsdk/2.10.19",
        "minizip/1.3.1",
        "zlib/1.3.1",
        "nlohmann_json/3.12.0",
        "fmt/11.2.0",
    )

    def generate(self):
        CMakeDeps(self).generate()
        toolchain = CMakeToolchain(self)
        toolchain.user_presets_path = False
        toolchain.generate()
