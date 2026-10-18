"""Validate the installed setup contract using only the bundled helper modules."""
import layout_migration
import graphical_setup


def check():
    import sys
    from pathlib import Path
    sys.path.insert(0,str(Path(__file__).parent/'mod_import'))
    import generate, model, voice, binary
    data=layout_migration.policy(graphical_setup.RESOURCES)
    for relative in data['retired_profiles']:
        suffix=relative.removeprefix('config/mm3d-local/')
        if (graphical_setup.RESOURCES/'defaults'/suffix).exists():
            raise ValueError('Retired profile is still packaged: '+suffix)
    print('Packaged setup imports, layout policy verified.')


if __name__=='__main__':
    check()
