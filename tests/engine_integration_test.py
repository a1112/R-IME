"""Real librime + daemon integration; no mock dictionary or transport."""
import concurrent.futures, json, os, pathlib, platform, shutil, socket, subprocess, sys, tempfile, time, unittest

WORKER = sys.argv.pop(1) if len(sys.argv) > 1 else 'build/src/engine/r-ime-engine'
OVERLAY = sys.argv.pop(1) if len(sys.argv) > 1 else 'data/rime'

class EngineIntegration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='r-ime-test-')
        cls.root = pathlib.Path(cls.temp.name)
        cls.env = dict(os.environ, XDG_RUNTIME_DIR=str(cls.root), XDG_DATA_HOME=str(cls.root/'data'), XDG_CONFIG_HOME=str(cls.root/'config'))
        cls.log = open(cls.root/'engine.log', 'w+')
        cls.overlay=cls.root/'overlay';shutil.copytree(OVERLAY,cls.overlay)
        cls.start_worker()

    @classmethod
    def start_worker(cls):
        cls.sock_path = cls.root/'r-ime/engine.sock'
        cls.sock_path.unlink(missing_ok=True)
        cls.proc = subprocess.Popen([WORKER, '--overlay', str(cls.overlay)], env=cls.env, stdout=cls.log, stderr=cls.log)
        cls.sock_path = cls.root/'r-ime/engine.sock'
        for _ in range(1200):
            if cls.sock_path.exists(): break
            if cls.proc.poll() is not None:
                cls.log.seek(0); raise RuntimeError(cls.log.read())
            time.sleep(.05)
        cls.sock = socket.socket(socket.AF_UNIX); cls.sock.settimeout(60); cls.sock.connect(str(cls.sock_path))
        cls.stream = cls.sock.makefile('rwb', buffering=0)
        cls.seq = 0

    @classmethod
    def stop_worker(cls):
        cls.stream.close(); cls.sock.close(); cls.proc.terminate()
        try: cls.proc.wait(10)
        except subprocess.TimeoutExpired: cls.proc.kill(); cls.proc.wait()

    @classmethod
    def tearDownClass(cls):
        cls.stop_worker()
        cls.log.seek(0)
        print('Engine diagnostics:\n'+cls.log.read()[-12000:],flush=True)
        cls.log.close(); cls.temp.cleanup()

    def req(self, op, session=0, **fields):
        type(self).seq += 1
        req = dict(version=1, id=type(self).seq, session=session, op=op, **fields)
        self.stream.write((json.dumps(req)+'\n').encode())
        response=json.loads(self.stream.readline())
        self.assertEqual(response['id'],req['id'])
        return response

    def create(self):
        response=self.req('create'); self.assertTrue(response['ok'], response); return response['session']

    def type_keys(self, session, keys):
        for char in keys: result=self.req('key', session, keycode=ord(char), modifiers=0)
        return result

    def test_01_actual_pinyin_raw_enter_and_isolation(self):
        a,b=self.create(),self.create()
        result=self.type_keys(a,'nihao'); self.assertTrue(result['candidates'],result)
        self.assertEqual(self.req('status',b)['preedit'],'')
        raw=self.req('key',a,keycode=0xff0d,modifiers=0)
        self.assertEqual(raw['commit'],'nihao'); self.assertEqual(raw['preedit'],'')
        self.type_keys(a,'nihao'); selected=self.req('select',a,index=0)
        self.assertEqual(selected['commit'],'你好')
        self.req('destroy',a);self.req('destroy',b)

    def test_02_double_pinyin_and_simplification(self):
        for schema,keys in [('rime_ime_flypy','nihc'),('rime_ime_ziranma','nihk')]:
            session=self.create(); self.assertTrue(self.req('set_schema',session,schema=schema)['ok'])
            result=self.type_keys(session,keys);self.assertTrue(result['candidates'],result)
            self.assertEqual(self.req('select',session,index=0)['commit'],'你好')
            self.req('destroy',session)
        session=self.create();self.type_keys(session,'hanyu')
        simp=self.req('select',session,index=0)['commit'];self.assertIn('汉',simp)
        self.req('toggle_simplified',session);self.type_keys(session,'hanyu')
        trad=self.req('select',session,index=0)['commit'];self.assertIn('漢',trad)
        self.req('destroy',session)

    def test_03_paging_reset_and_protocol_errors(self):
        session=self.create();first=self.type_keys(session,'shi')
        self.assertEqual(len(first['candidates']),5)
        page=self.req('page',session,backward=False);self.assertGreater(page['page'],first['page'])
        self.req('reset',session);self.assertEqual(self.req('status',session)['preedit'],'')
        self.assertFalse(self.req('select',session,index=-1)['ok'])
        self.assertFalse(self.req('key',999999999,keycode=ord('a'),modifiers=0)['ok'])
        self.req('destroy',session)
        self.assertEqual(self.sock_path.stat().st_mode & 0o777,0o600)
        self.assertEqual(self.sock_path.parent.stat().st_mode & 0o777,0o700)

    def test_04_sensitive_and_ascii_shortcuts_pass_through(self):
        result=self.req('create',sensitive=True);s=result['session']
        result=self.type_keys(s,'abc');self.assertEqual(result['preedit'],'');self.assertFalse(result['handled'])
        self.req('destroy',s)
        s=self.create();result=self.req('key',s,keycode=ord('c'),modifiers=4)
        self.assertFalse(result['handled']);self.assertEqual(result['preedit'],'')
        self.type_keys(s,'ni');result=self.req('toggle_ascii',s)
        self.assertEqual(result['commit'],'ni');self.assertTrue(result['ascii']);self.req('destroy',s)

    def test_05_configure_phrase_backup_restore_and_rollback(self):
        original=self.req('status')['config'];updated=json.loads(json.dumps(original))
        updated['appearance']['page_size']=7
        result=self.req('configure',config=updated,phrases='rime\t我的定制输入法\t100\n')
        self.assertTrue(result['ok'],result)
        s=self.create();result=self.type_keys(s,'rime')
        self.assertEqual(result['candidates'][0]['text'],'我的定制输入法')
        self.req('reset',s);result=self.type_keys(s,'shi');self.assertEqual(len(result['candidates']),7)
        archive=self.req('backup');self.assertTrue(archive['ok'],archive)
        self.assertTrue(any('.userdb/' in item['path'] for item in archive['backup']['files']),
                        'enabled learning produced no persistent user dictionary')
        # Invalidate input handles across generations, never deliver a new app's
        # composition under a previous handle.
        self.assertFalse(self.req('status',s)['ok'])
        missing=self.overlay/'rime_ime_flypy.schema.yaml';saved=missing.read_bytes();missing.unlink()
        try:
            failed=self.req('configure',config=original)
            self.assertFalse(failed['ok']);self.assertEqual(self.req('status')['config'],updated)
            s=self.create();self.assertTrue(self.type_keys(s,'nihao')['candidates']);self.req('destroy',s)
        finally:missing.write_bytes(saved)
        self.assertTrue(self.req('configure',config=original,phrases='')['ok'])
        result=self.req('restore',backup=archive['backup']);self.assertTrue(result['ok'],result)
        self.assertEqual(self.req('status')['config'],updated)
        invalid=json.loads(json.dumps(archive['backup']));invalid['files'][0]['path']='../escape'
        self.assertFalse(self.req('restore',backup=invalid)['ok'])
        s=self.create();self.assertTrue(self.type_keys(s,'nihao')['candidates']);self.req('destroy',s)
        self.assertTrue(self.req('configure',config=original,phrases='')['ok'])

    def test_06_session_ownership_and_stale_sequence(self):
        s=self.create()
        other=socket.socket(socket.AF_UNIX);other.settimeout(2);other.connect(str(self.sock_path))
        other.sendall((json.dumps(dict(version=1,id=1,session=s,op='status'))+'\n').encode())
        response=json.loads(other.recv(4096));self.assertFalse(response['ok']);other.close()
        duplicate=dict(version=1,id=type(self).seq,session=s,op='status')
        self.stream.write((json.dumps(duplicate)+'\n').encode());self.assertFalse(json.loads(self.stream.readline())['ok'])
        self.req('destroy',s)

    def test_07_navigation_deletion_cancel_space_and_learning(self):
        s=self.create();result=self.type_keys(s,'nihaox')
        result=self.req('key',s,keycode=0xff08,modifiers=0)
        self.assertTrue(result['candidates']);self.assertEqual(self.req('key',s,keycode=32,modifiers=0)['commit'],'你好')
        self.type_keys(s,'shi');result=self.req('key',s,keycode=0xff54,modifiers=0)
        self.assertEqual(result['highlighted'],1)
        result=self.req('key',s,keycode=0xff52,modifiers=0);self.assertEqual(result['highlighted'],0)
        result=self.req('key',s,keycode=0xff1b,modifiers=0);self.assertEqual(result['preedit'],'')
        self.req('destroy',s)
        original=self.req('status')['config'];disabled=json.loads(json.dumps(original));disabled['general']['learning']=False
        self.assertTrue(self.req('configure',config=disabled)['ok'])
        before=self.req('backup')['backup']
        s=self.create();self.type_keys(s,'dingzhi');self.req('select',s,index=0);self.req('destroy',s)
        after=self.req('backup')['backup']
        def databases(archive):
            # LevelDB rotates LOG/MANIFEST even on a read-only reopen. Compare
            # exported learned entries, not database housekeeping bytes.
            with tempfile.TemporaryDirectory(prefix='r-ime-dict-') as directory:
                root=pathlib.Path(directory)
                for item in archive['files']:
                    if '.userdb/' in item['path']:
                        path=root/item['path'];path.parent.mkdir(parents=True,exist_ok=True)
                        path.write_bytes(bytes.fromhex(item['hex']))
                result={}
                for db in (root/'rime').glob('*.userdb'):
                    output=root/(db.name+'.txt')
                    subprocess.run(['rime_dict_manager','-e',db.name[:-7],str(output)],cwd=db.parent,check=True,capture_output=True)
                    result[db.name]=sorted(line for line in output.read_text().splitlines() if line and not line.startswith('#'))
                return result
        self.assertEqual(databases(before),databases(after),'disabled learning changed user dictionary')
        self.assertTrue(self.req('configure',config=original)['ok'])

    def test_08_warm_engine_latency(self):
        s=self.create();before=self.type_keys(s,'nihao')
        original=self.req('status')['config'];appearance=json.loads(json.dumps(original));appearance['appearance']['theme']='dark'
        applied=self.req('configure',config=appearance);self.assertTrue(applied['ok'],applied)
        self.assertFalse(applied['sessions_invalidated'],'appearance edit unnecessarily invalidated composition')
        self.assertEqual(self.req('status',s)['preedit'],before['preedit'])
        self.req('configure',config=original);self.req('reset',s)
        timings=[]
        for _ in range(40):
            for char in 'nihao':
                start=time.perf_counter();self.req('key',s,keycode=ord(char),modifiers=0)
                timings.append((time.perf_counter()-start)*1000)
            self.req('reset',s)
        p95=sorted(timings)[int(len(timings)*.95)-1]
        print(f'Warm engine Unix IPC P95: {p95:.2f} ms; {platform.platform()}',flush=True)
        self.assertLess(p95,30,'warm worker/IPC exceeds the 30 ms budget (UI latency must be measured separately)')
        self.req('destroy',s)

    def test_09_deployment_serves_existing_input(self):
        original=self.req('status')['config'];updated=json.loads(json.dumps(original));updated['appearance']['page_size']=6
        session=self.create();self.type_keys(session,'ni')
        def deploy():
            with socket.socket(socket.AF_UNIX) as control:
                control.settimeout(120);control.connect(str(self.sock_path))
                control.sendall((json.dumps(dict(version=1,id=1,session=0,op='configure',config=updated))+'\n').encode())
                with control.makefile('rb') as stream:return json.loads(stream.readline())
        with concurrent.futures.ThreadPoolExecutor() as pool:
            future=pool.submit(deploy);responses=0
            while not future.done():
                result=self.req('status',session)
                if result['ok']:
                    self.assertTrue(result['preedit']);responses+=1
                else:self.assertEqual(result['error'],'unknown or expired session')
                time.sleep(.005)
            result=future.result();self.assertTrue(result['ok'],result)
        self.assertGreater(responses,0,'deployment blocked all existing input requests')
        self.assertTrue(self.req('configure',config=original)['ok'])

    def test_10_restart_recovers_interrupted_swap_and_corrupt_external_config(self):
        original=self.req('status')['config'];updated=json.loads(json.dumps(original));updated['appearance']['page_size']=8
        cls=type(self);cls.stop_worker()
        user=self.root/'data/r-ime/rime';old=user.parent/'.rime-old-test'
        user.rename(old);shutil.copytree(old,user)
        (user/'.transaction-committed').unlink(missing_ok=True)
        (user/'r-ime-effective.json').write_text(json.dumps(updated))
        (self.root/'config/r-ime/config.json').write_text(json.dumps(updated))
        cls.start_worker();self.assertEqual(self.req('status')['config'],original)
        s=self.create();self.assertEqual(self.type_keys(s,'nihao')['candidates'][0]['text'],'你好');self.req('destroy',s)
        cls.stop_worker();(self.root/'config/r-ime/config.json').write_text('{broken')
        cls.start_worker();self.assertEqual(self.req('status')['config'],original)
        s=self.create();self.assertTrue(self.type_keys(s,'shi')['candidates']);self.req('destroy',s)

    def test_11_corrupt_first_config_can_explicitly_repair_without_erasing_phrases(self):
        original=self.req('status')['config'];cls=type(self);cls.stop_worker()
        user=self.root/'data/r-ime/rime';effective=user/'r-ime-effective.json'
        effective.rename(user/'r-ime-effective.saved')
        config=self.root/'config/r-ime/config.json';config.write_text('{damaged')
        phrases=self.root/'data/r-ime/phrases.tsv';phrases.write_text('ceshi\t保留的短语\t100\n')
        cls.start_worker();self.assertTrue(self.req('status')['recovery_required'])
        self.assertFalse(self.req('configure',config=original)['ok'])
        result=self.req('configure',config=original,repair_config=True);self.assertTrue(result['ok'],result)
        self.assertEqual(pathlib.Path(str(config)+'.invalid').read_text(),'{damaged')
        self.assertEqual(phrases.read_text(),'ceshi\t保留的短语\t100\n')
        s=self.create();self.assertEqual(self.type_keys(s,'ceshi')['candidates'][0]['text'],'保留的短语');self.req('destroy',s)

if __name__=='__main__': unittest.main()
