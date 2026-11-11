from esphome.loader import FileResource
from tests.testing_helpers import ComponentManifestOverride


def override_manifest(manifest: ComponentManifestOverride) -> None:
    # host must run its to_code during C++ test builds because it sets up the
    # host platform target, which is the execution environment for all unit tests.
    manifest.enable_codegen()
    # The portable XR806 FDCM journal is tested on the host; do not pull in
    # the vendor-specific XR806 C++ sources, only its standalone header.
    manifest.add_resources(
        FileResource("esphome.components.xr806", "preference_journal.h")
    )
