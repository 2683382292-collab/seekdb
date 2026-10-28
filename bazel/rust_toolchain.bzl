"""Expose the project's installed Rust SDK to native Bazel Rust rules."""

load("@rules_rust//cargo:cargo_toolchain.bzl", "cargo_toolchain")
load("@rules_rust//rust:rust_stdlib_filegroup.bzl", "rust_stdlib_filegroup")
load("@rules_rust//rust:rust_toolchain.bzl", "rust_toolchain")
load("@rules_rust//rust/platform:triple.bzl", "triple")
load(
    "@rules_rust//rust/platform:triple_mappings.bzl",
    "system_to_binary_ext",
    "system_to_dylib_ext",
    "system_to_staticlib_ext",
    "system_to_stdlib_linkflags",
    "triple_to_constraint_set",
)

RUST_TARGETS = [
    "x86_64-unknown-linux-gnu",
    "aarch64-unknown-linux-gnu",
    "x86_64-apple-darwin",
    "aarch64-apple-darwin",
    "x86_64-pc-windows-msvc",
    "aarch64-pc-windows-msvc",
    "aarch64-linux-android",
    "aarch64-apple-ios",
    "aarch64-apple-ios-sim",
]

def declare_installed_rust(host, version, targets):
    """Declare only target standard libraries already installed by init."""
    host_info = triple(host)
    suffix = system_to_binary_ext(host_info.system)
    host_constraints = triple_to_constraint_set(host)
    native.exports_files(["BUILD.bazel", "bin/cargo" + suffix])
    native.filegroup(
        name = "compiler_libraries",
        srcs = native.glob([
            "bin/*.dll",
            "lib/*.so*",
            "lib/*.dylib",
            "lib/rustlib/%s/codegen-backends/**" % host,
            "lib/rustlib/%s/lib/*.so" % host,
            "lib/rustlib/%s/lib/*.dylib" % host,
        ], allow_empty = True),
    )
    cargo_toolchain(
        name = "cargo_impl",
        cargo = "bin/cargo" + suffix,
        version = version,
        channel = "stable",
    )
    for target in targets:
        target_info = triple(target)
        name = target.replace("-", "_")
        constraints = triple_to_constraint_set(target)
        rust_stdlib_filegroup(
            name = name + "_std",
            srcs = native.glob([
                "lib/rustlib/%s/lib/*.rlib" % target,
                "lib/rustlib/%s/lib/*.rmeta" % target,
                "lib/rustlib/%s/lib/*.so" % target,
                "lib/rustlib/%s/lib/*.dylib" % target,
                "lib/rustlib/%s/lib/self-contained/**" % target,
            ], allow_empty = True),
        )
        rust_toolchain(
            name = name + "_impl",
            rustc = "bin/rustc" + suffix,
            rust_doc = "bin/rustdoc" + suffix,
            rustc_lib = ":compiler_libraries",
            rust_std = ":" + name + "_std",
            exec_triple = host,
            target_triple = target,
            version = version,
            channel = "stable",
            binary_ext = system_to_binary_ext(target_info.system),
            dylib_ext = system_to_dylib_ext(target_info.system),
            staticlib_ext = system_to_staticlib_ext(target_info.system, target_info.abi),
            stdlib_linkflags = system_to_stdlib_linkflags(target_info.system, target_info.abi),
            default_edition = "2021",
            opt_level = {"dbg": "3", "fastbuild": "3", "opt": "3"},
            debug_info = {"dbg": "2", "fastbuild": "2", "opt": "2"},
            strip_level = {"dbg": "none", "fastbuild": "none", "opt": "none"},
            extra_rustc_flags = ["-Cpanic=abort"],
            env = {},
        )
        for kind, implementation, toolchain_type in [
            ("rust", name + "_impl", "@rules_rust//rust:toolchain_type"),
            ("cargo", "cargo_impl", "@rules_rust//cargo:toolchain_type"),
        ]:
            native.toolchain(
                name = name + "_" + kind,
                toolchain = ":" + implementation,
                toolchain_type = toolchain_type,
                exec_compatible_with = host_constraints,
                target_compatible_with = constraints,
                target_settings = ["@rules_rust//rust/toolchain/channel:stable"],
            )

def _installed_rust_repository_impl(ctx):
    pin = ctx.read(ctx.attr.pin)
    channel = [line.split('"')[1] for line in pin.splitlines() if line.startswith("channel =")][0]
    rustup = ctx.which("rustup")
    if not rustup:
        fail("Run ./build.py init to install the pinned Rust toolchain")
    result = ctx.execute([rustup, "which", "rustc", "--toolchain", channel], environment = {"RUSTUP_AUTO_INSTALL": "0"})
    if result.return_code:
        fail("Run ./build.py init to install Rust %s:\n%s" % (channel, result.stderr))
    rustc = ctx.path(result.stdout.strip())
    root = rustc.dirname.dirname
    ctx.watch(root)
    ctx.watch(rustc)
    details = ctx.execute([rustc, "-vV"])
    if details.return_code:
        fail("Cannot inspect the installed Rust compiler:\n" + details.stderr)
    fields = dict([line.split(": ", 1) for line in details.stdout.splitlines() if ": " in line])
    for entry in root.readdir():
        ctx.symlink(entry, entry.basename)
    targets = [
        target for target in RUST_TARGETS if root.get_child("lib", "rustlib", target, "lib").exists
    ]
    ctx.watch(root.get_child("lib", "rustlib"))
    ctx.file("BUILD.bazel", '''load("@seekdb//bazel:rust_toolchain.bzl", "declare_installed_rust")
package(default_visibility = ["//visibility:public"])
declare_installed_rust(host = %r, version = %r, targets = %r)
''' % (fields["host"], fields["release"], targets))

installed_rust_repository = repository_rule(
    implementation = _installed_rust_repository_impl,
    attrs = {
        "pin": attr.label(default = Label("//:rust-toolchain.toml")),
    },
    environ = ["PATH", "RUSTUP_HOME", "CARGO_HOME", "HOME"],
    local = True,
    configure = True,
)
