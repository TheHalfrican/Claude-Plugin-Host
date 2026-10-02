"""Claude Bridge: a Live Remote Script giving Claude the controls AbletonMCP
lacks (mixer, routing, sidechain sources, deleting/duplicating, clip
automation). Select "ClaudeBridge" as a Control Surface in Live's settings.

The Live-specific module is imported only when Live creates the instance, so
commands and server can be imported and tested outside Live.
"""


def create_instance(c_instance):
    from .bridge import ClaudeBridge
    return ClaudeBridge(c_instance)
