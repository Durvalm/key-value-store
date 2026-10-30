"""Publication preserves the old configuration on refusal or interruption."""
from contextlib import ExitStack
import fcntl
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading

from kv_layout_integration_test import make_groups, write_layout, start_node
from kv_verification_test import save_logs


def run(activate, old, new, active):
    return subprocess.run([str(activate), str(old), str(new), str(active)],
                          text=True, capture_output=True, timeout=15)


def test_publication(server, activate, change):
    with tempfile.TemporaryDirectory() as directory, ExitStack() as cleanup:
        directory = Path(directory)
        (directory/'old').mkdir()
        (directory/'new').mkdir()
        (directory/'published').mkdir()
        source, target = make_groups(directory/'old', 1)[0], make_groups(directory/'new', 1)[0]
        target['history'] = 'new-history'
        old, new = directory/'old.layout', directory/'new.layout'
        write_layout(old, [source])
        write_layout(new, [target])
        save_logs(source, {'key': 'value'})
        save_logs(target, {'key': 'wrong' if change == 'value' else 'value'})
        start_node(server, new, target, 'replica', cleanup)
        start_node(server, new, target, 'primary', cleanup)
        active = directory/'published'/'active.layout'
        active.write_bytes(old.read_bytes())
        if change == 'mismatch':
            active.write_bytes(new.read_bytes())
        elif change == 'alias':
            active.unlink()
            active.hardlink_to(source['primary_log'])
        elif change == 'lock-alias':
            Path(str(active)+'.lock').hardlink_to(new)
        elif change == 'symlink':
            active.unlink()
            active.symlink_to(old)
        elif change == 'input':
            active = new
        elif change == 'missing-parent':
            active = directory/'missing'/'active.layout'
        elif change == 'absent':
            active.unlink()
        elif change == 'locked':
            lock = cleanup.enter_context(open(str(active)+'.lock', 'a'))
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        # Relative candidate log paths must become absolute in relocated output.
        relative = dict(target, primary_log='new/primary-0.log', replica_log='new/replica-0.log')
        write_layout(new, [relative])
        protected = [old, new] + [Path(g[f'{r}_log']) for g in [source, target] for r in ['primary','replica']]
        before = {p: p.read_bytes() for p in protected}
        prior = active.read_bytes() if active.exists() else None
        result = run(activate, old, new, active)
        if change in ['correct', 'absent']:
            assert result.returncode == 0, result.stderr
            output = active.read_text()
            assert 'new-history' in output
            assert str(Path(target['primary_log']).resolve()) in output
            assert str(Path(target['replica_log']).resolve()) in output
            # Re-running with an obsolete source must not replace the active layout.
            again = run(activate, old, new, active)
            assert again.returncode != 0 and 'does not match the source' in again.stderr, again.stderr
            assert active.read_text() == output
        else:
            assert result.returncode != 0 and 'published active layout' not in result.stdout, result.stdout
            assert (active.read_bytes() if active.exists() else None) == prior
        assert all(p.read_bytes() == contents for p, contents in before.items())
        assert not list(active.parent.glob('active.layout.tmp-*'))


def test_interruption(server, activate):
    with tempfile.TemporaryDirectory() as directory, ExitStack() as cleanup:
        directory = Path(directory)
        (directory/'old').mkdir()
        (directory/'new').mkdir()
        source, target = make_groups(directory/'old', 1)[0], make_groups(directory/'new', 1)[0]
        target['history'] = 'new-history'
        old, new, active = directory/'old.layout', directory/'new.layout', directory/'active.layout'
        write_layout(old, [source])
        write_layout(new, [target])
        active.write_bytes(old.read_bytes())
        save_logs(source, {'key': 'value'})
        save_logs(target, {'key': 'value'})
        # Pause during verification, before publication. No timing-only sleep.
        reached, release, errors = threading.Event(), threading.Event(), []
        with socket.socket() as primary, socket.socket() as replica:
            for listener, role in [(primary,'primary'), (replica,'replica')]:
                listener.bind(('127.0.0.1', target[f'{role}_port']))
                listener.listen(1)
            primary.settimeout(5)

            def pause():
                try:
                    connection, _ = primary.accept()
                    with connection, connection.makefile('rb') as stream:
                        connection.settimeout(5)
                        assert stream.readline().startswith(b'VERIFY_ACTIVATE ')
                        reached.set()
                        assert release.wait(5)
                except BaseException as error:
                    errors.append(error)
                    reached.set()

            thread = threading.Thread(target=pause)
            thread.start()
            process = subprocess.Popen([str(activate), str(old), str(new), str(active)],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                assert reached.wait(5) and not errors, errors
                assert active.read_bytes() == old.read_bytes()
                concurrent = run(activate, old, new, active)
                assert concurrent.returncode != 0 and 'in use' in concurrent.stderr, concurrent.stderr
                process.terminate()
                stdout, stderr = process.communicate(timeout=5)
                assert process.returncode != 0 and 'published active layout' not in stdout
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=5)
                release.set()
                thread.join(timeout=6)
            assert not thread.is_alive() and not errors, errors
        assert active.read_bytes() == old.read_bytes()
        # Kernel releases both publication and source leases on process exit.
        start_node(server, new, target, 'replica', cleanup)
        start_node(server, new, target, 'primary', cleanup)
        result = run(activate, old, new, active)
        assert result.returncode == 0, result.stderr


if __name__ == '__main__':
    server, activate = map(Path, sys.argv[1:])
    for change in ['correct', 'absent', 'value', 'mismatch', 'alias', 'lock-alias', 'symlink', 'input', 'missing-parent', 'locked']:
        test_publication(server, activate, change)
    test_interruption(server, activate)
    print('Activation publication, failure preservation, relocation and interruption checks passed.')
