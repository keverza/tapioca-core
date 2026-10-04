"""Shared calculations for the native Massing HUD and retained Python commands.

No UI, model writes or command lifetime here. Native owners supply immutable
geometry, call the calculation on a worker, and publish only current results.
"""
