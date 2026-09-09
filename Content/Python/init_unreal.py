"""Project Python startup registrations."""

try:
    import bf6_water_mcp
    bf6_water_mcp.register()
except Exception as exc:
    import unreal
    unreal.log_error(f"BF6 water MCP registration failed: {exc}")

# Registered separately, and its failure must not take the water toolset with
# it. This one reads the tool's own inventory, logs and gap report, so it is the
# toolset that answers "what is the editor doing" without a grep through
# Saved/Logs afterwards.
try:
    import bf6_context_mcp
    bf6_context_mcp.register()
except Exception as exc:
    import unreal
    unreal.log_error(f"BF6 context MCP registration failed: {exc}")
