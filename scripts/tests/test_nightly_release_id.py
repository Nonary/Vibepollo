import json, os, pathlib, subprocess, tempfile, unittest

class ReleaseIdTests(unittest.TestCase):
    def check(self, data, expected):
        text = (pathlib.Path(__file__).resolve().parents[2] / '.github/workflows/daily-prerelease.yml').read_text()
        start = text.index('          created_release_json=')
        end = text.index('          gh release upload',start)
        script='set -euo pipefail\n'+ '\n'.join(line[10:] for line in text[start:end].splitlines())+'\nprintf "%s" "$created_release_id"\n'
        with tempfile.TemporaryDirectory() as d:
            gh=pathlib.Path(d)/'gh'
            gh.write_text('#!/bin/sh\n[ "$1" = release ] && [ "$2" = view ] || exit 2\nprintf "%s" "$FIXTURE"\n')
            gh.chmod(0o755)
            p=subprocess.run(['bash','-c',script],env=dict(os.environ,PATH=d+':'+os.environ['PATH'],FIXTURE=json.dumps(data),GITHUB_REPOSITORY='Nonary/release-test',DAILY_TAG='2.2.2'),capture_output=True,text=True)
            if expected: self.assertEqual((p.returncode,p.stdout),(0,'123'))
            else: self.assertNotEqual(p.returncode,0)
    def test_numeric_rest_id(self): self.check(dict(databaseId=123,id='RE_x',isDraft=True,tagName='2.2.2'),True)
    def test_node_id_rejected(self): self.check(dict(databaseId='RE_x',isDraft=True,tagName='2.2.2'),False)
    def test_wrong_tag(self): self.check(dict(databaseId=123,isDraft=True,tagName='2.2.3'),False)
    def test_published_rejected(self): self.check(dict(databaseId=123,isDraft=False,tagName='2.2.2'),False)
    def test_missing_id(self): self.check(dict(isDraft=True,tagName='2.2.2'),False)
    def test_fraction_id(self): self.check(dict(databaseId=1.5,isDraft=True,tagName='2.2.2'),False)
    def test_zero_id(self): self.check(dict(databaseId=0,isDraft=True,tagName='2.2.2'),False)
if __name__=='__main__': unittest.main()
