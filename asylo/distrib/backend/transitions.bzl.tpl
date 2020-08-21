"""Starlark support for backend and toolchain transitions."""

load("@com_google_asylo_backend_provider//:enclave_info.bzl", "backend_tools")
load("@rules_cc//cc:defs.bzl", "cc_binary")

BACKEND_LABEL = "@com_google_asylo_backend_provider//:backend"
PRETRANSITION_TAGS = [
    "asylo-pretransition",
    "manual",
]

def _placeholder(**kwargs):
    fail("Transition rules in external repositories are unsupported in Bazel.")

def _empty_transition_impl(settings, attr):
    _ignore = (settings, attr)
    return {}

empty_transition = _placeholder

def _asylo_toolchain_and_backend_transition_impl(settings, attr):
    """Returns the configuration to use the Asylo toolchain."""
    result = {
        "//command_line_option:crosstool_top": "@com_google_asylo_toolchain//toolchain:crosstool",
        "//command_line_option:custom_malloc": "@com_google_asylo_toolchain//toolchain:malloc",
        "//command_line_option:dynamic_mode": "off",
        "//command_line_option:host_crosstool_top": "@bazel_tools//tools/cpp:toolchain",
        BACKEND_LABEL: attr.backend or settings[BACKEND_LABEL],
    }
    return result

asylo_toolchain_transition = _placeholder

def _asylo_backend_transition_impl(settings, attr):
    _ignore = (settings)
    return {BACKEND_LABEL: attr.backend}

asylo_backend_transition = _placeholder

def _forward_target_transition(ctx, executable):
    """Copies cc_target output from a transitioned toolchain to the host.

    Args:
        ctx: Starlark context object.
        executable: True iff the cc_target is executable.

    Returns:
        List of providers.
    """

    # The transition could be a 1:N split transition, but in this case there is
    # only 1 transition target. Thus index by 0 here.
    split_target = ctx.attr.cc_target[0]
    binary_output = ctx.actions.declare_file(ctx.label.name)

    # Copy the cc_target to the expected output location since an executable
    # rule must produce its own executable file.
    ctx.actions.run_shell(
        inputs = [ctx.file.cc_target],
        command = "cp {} {}".format(ctx.file.cc_target.path, binary_output.path),
        outputs = [binary_output],
    )
    new_runfiles = ctx.runfiles(
        files = [binary_output],
        transitive_files = split_target[DefaultInfo].data_runfiles.files,
    )
    result = [
        DefaultInfo(
            files = depset([binary_output]),
            data_runfiles = new_runfiles,
            default_runfiles = new_runfiles,
            executable = binary_output if executable else None,
        ),
        OutputGroupInfo(bin = depset([binary_output])),
    ]

    # Forward the EnclaveInfo provider to allow enclaves to flow to enclave
    # rule positions.
    if ctx.attr.backend:
        backend_info = ctx.attr.backend[backend_tools.AsyloBackendInfo]
        for provider in backend_info.forward_providers:
            if provider in split_target:
                result.append(split_target[provider])
    return result

# All of the following function definitions are to work around starlark's lack
# of lambdas.

def _forward_target_transition_executable(ctx):
    return _forward_target_transition(ctx, executable = True)

def _forward_target_transition_library(ctx):
    return _forward_target_transition(ctx)

def _with_transition_impl(executable):
    if executable:
        return _forward_target_transition_executable
    return _forward_target_transition_library

_BACKEND_DOC = "The Asylo backend label"

def _make_transition_forwarding_rule(
        executable = False,
        test = False,
        cfg = asylo_toolchain_transition,
        transition_doc = "an Asylo toolchain and optional backend transition",
        extra_attrs = {}):
    """Returns a rule that applies a transition to a cc_target and copies it through.

    Args:
        executable: True iff the rule defines an executable.
        test: True iff the rule defines a test.
        cfg: The transition to use for cc_target.
        transition_doc: A piece of documentation text for the rule about the
            kind of transition applied on cc_target.
        extra_attrs: A dictionary of argument name to attribute, used to extend
            the attributes provided by the defined rule. Base attributes are
            cc_target, backend, and _whitelist_function_transition.

    Returns:
        A starlark rule object.
    """
    attrs = {
        "cc_target": attr.label(
            doc = "The target to forward through the transition",
            cfg = cfg,
            allow_single_file = True,
            providers = [DefaultInfo],
            mandatory = True,
        ),
        "_whitelist_function_transition": attr.label(
            default = "//tools/whitelists/function_transition_whitelist",
        ),
    }
    attrs["backend"] = attr.label(
        doc = _BACKEND_DOC,
        providers = [backend_tools.AsyloBackendInfo],
    )
    attrs.update(**extra_attrs)

    has_executable = executable or test
    kind_doc = "cc_library"
    if test:
        kind_doc = "cc_test"
    elif executable:
        kind_doc = "cc_binary"
    return rule(
        doc = "Copies a {} target through {}".format(kind_doc, transition_doc) +
              (" and forwards runfile info and" + " backend-relevant providers."),
        implementation = _with_transition_impl(has_executable),
        executable = has_executable,
        test = test,
        attrs = attrs,
    )

def _make_asylo_backend_rule(executable = False, test = False):
    return _make_transition_forwarding_rule(
        executable = executable,
        test = test,
        cfg = asylo_backend_transition,
        transition_doc = "an explicit backend transition",
    )

_UNSUPPORTED_PKGS = [
    # Bazel (v1.0.1) does not support transitions from external dependencies,
    # which com_google_asylo_backend_provider is, so no packages are supported
    # yet.
    "",
]

def transitions_supported(package_name):
    """Returns true if a given package has been converted to use transitions.

    Args:
        package_name: The package name that is using an enclave rule.

    Returns:
        True only if package_name is expected to work with transitions.
    """
    for pkg in _UNSUPPORTED_PKGS:
        if package_name.startswith(pkg):
            return False
    return True

with_asylo_binary = _placeholder
with_asylo_test = _placeholder
with_asylo_library = _placeholder

with_backend_binary = _placeholder
with_backend_test = _placeholder
with_backend_library = _placeholder

def _cc_backend_binary_impl(ctx):
    return backend_tools.cc_binary(
        ctx,
        ctx.label.name,
        extra_data = ctx.files.backend_dependent_data,
        extra_deps = [ctx.attr._stl],
    )

_cc_backend_binary = _placeholder

def _cc_binary(name, backends = backend_tools.should_be_all_backends, name_by_backend = {}, **kwargs):
    if transitions_supported(native.package_name()):
        backend_tools.all_backends(
            rule_or_macro = _cc_backend_binary,
            name = name,
            name_by_backend = name_by_backend,
            backends = backends,
            kwargs = kwargs,
        )
    else:
        cc_binary(
            name = name,
            data = kwargs.pop("data", []) + kwargs.pop("backend_dependent_data", []),
            **kwargs
        )

transitions = struct(
    toolchain = asylo_toolchain_transition,
    backend = asylo_backend_transition,
    cc_binary = _cc_binary,
    asylo_binary = with_asylo_binary,
    asylo_test = with_asylo_test,
    asylo_library = with_asylo_library,
    backend_binary = with_backend_binary,
    backend_test = with_backend_test,
    backend_library = with_backend_library,
    empty_transition = empty_transition,
    make_rule = _make_transition_forwarding_rule,
    pre_tags = PRETRANSITION_TAGS,
    supported = transitions_supported,
)