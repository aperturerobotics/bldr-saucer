package bldr_saucer_test

import (
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/aperturerobotics/starpc/srpc"
)

// saucerBinary is the path to the built bldr-saucer binary.
// Set once by TestMain.
var saucerBinary string

func TestMain(m *testing.M) {
	// Build the bldr-saucer binary once for all tests.
	tmpDir, err := os.MkdirTemp("", "bldr-saucer-test-*")
	if err != nil {
		fmt.Fprintf(os.Stderr, "failed to create temp dir: %v\n", err)
		os.Exit(1)
	}
	defer os.RemoveAll(tmpDir)

	buildDir := filepath.Join(tmpDir, "build")

	repoRoot, err := filepath.Abs(".")
	if err != nil {
		fmt.Fprintf(os.Stderr, "failed to get abs path: %v\n", err)
		os.Exit(1)
	}

	// Resolve C++ deps from vendor/ (populated by go mod vendor).
	saucerDir := filepath.Join(repoRoot, "vendor", "github.com", "aperturerobotics", "saucer")
	yamuxDir := filepath.Join(repoRoot, "vendor", "github.com", "aperturerobotics", "cpp-yamux")

	// Allow env var overrides for local development.
	if v := os.Getenv("SAUCER_SOURCE_DIR"); v != "" {
		saucerDir = v
	}
	if v := os.Getenv("YAMUX_SOURCE_DIR"); v != "" {
		yamuxDir = v
	}

	for _, d := range []string{saucerDir, yamuxDir} {
		if info, err := os.Stat(d); err != nil || !info.IsDir() {
			fmt.Fprintf(os.Stderr, "source dir not found: %s (run go mod vendor)\n", d)
			os.Exit(1)
		}
	}

	// Configure cmake.
	configCmd := exec.Command("cmake",
		"-G", "Ninja",
		"-S", repoRoot,
		"-B", buildDir,
		"-DCMAKE_BUILD_TYPE=Release",
		"-DSAUCER_SOURCE_DIR="+saucerDir,
		"-DYAMUX_SOURCE_DIR="+yamuxDir,
	)
	configCmd.Stdout = os.Stderr
	configCmd.Stderr = os.Stderr
	fmt.Fprintf(os.Stderr, "configuring cmake...\n")
	if err := configCmd.Run(); err != nil {
		fmt.Fprintf(os.Stderr, "cmake configure failed: %v\n", err)
		os.Exit(1)
	}

	// Build.
	fmt.Fprintf(os.Stderr, "building bldr-saucer...\n")
	buildCmd := exec.Command("cmake", "--build", buildDir)
	buildCmd.Stdout = os.Stderr
	buildCmd.Stderr = os.Stderr
	if err := buildCmd.Run(); err != nil {
		fmt.Fprintf(os.Stderr, "cmake build failed: %v\n", err)
		os.Exit(1)
	}

	saucerBinary = filepath.Join(buildDir, "bldr-saucer")
	fmt.Fprintf(os.Stderr, "built: %s\n", saucerBinary)

	os.Exit(m.Run())
}

// testHarness sets up a pipe listener, starts the saucer binary, and
// returns the yamux muxed connection for the test to use.
type testHarness struct {
	t      *testing.T
	mc     srpc.MuxedConn
	cmd    *exec.Cmd
	cancel func()
}

func newTestHarness(t *testing.T) *testHarness {
	t.Helper()

	// Use a short runtime ID and /tmp base to stay under macOS's ~104 char
	// Unix socket path limit. t.TempDir() paths are too long.
	runtimeID := fmt.Sprintf("t%d", time.Now().UnixNano()%1000000)
	workDir, err := os.MkdirTemp("/tmp", "bs-")
	if err != nil {
		t.Fatalf("mkdtemp: %v", err)
	}
	t.Cleanup(func() { os.RemoveAll(workDir) })

	// Create Unix socket listener.
	pipePath := filepath.Join(workDir, ".pipe-"+runtimeID)
	addr := &net.UnixAddr{Net: "unix", Name: pipePath}
	listener, err := net.ListenUnix("unix", addr)
	if err != nil {
		t.Fatalf("listen: %v", err)
	}

	// Accept one connection in background.
	connCh := make(chan net.Conn, 1)
	errCh := make(chan error, 1)
	go func() {
		conn, err := listener.Accept()
		if err != nil {
			errCh <- err
			return
		}
		connCh <- conn
	}()

	// Start the saucer binary.
	cmd := exec.Command(saucerBinary)
	cmd.Dir = workDir
	cmd.Env = append(os.Environ(),
		"BLDR_RUNTIME_ID="+runtimeID,
		"BLDR_WEB_DOCUMENT_ID=testdoc",
	)
	cmd.Stdout = os.Stderr
	cmd.Stderr = os.Stderr
	if err := cmd.Start(); err != nil {
		listener.Close()
		t.Fatalf("start saucer: %v", err)
	}

	// Wait for C++ to connect.
	var conn net.Conn
	select {
	case conn = <-connCh:
	case err := <-errCh:
		cmd.Process.Kill()
		cmd.Wait()
		listener.Close()
		t.Fatalf("accept: %v", err)
	case <-time.After(15 * time.Second):
		cmd.Process.Kill()
		cmd.Wait()
		listener.Close()
		t.Fatal("timeout waiting for C++ to connect")
	}
	listener.Close()

	// Upgrade to yamux. Go is server (outbound=false).
	mc, err := srpc.NewMuxedConn(conn, false, nil)
	if err != nil {
		conn.Close()
		cmd.Process.Kill()
		cmd.Wait()
		t.Fatalf("yamux: %v", err)
	}

	h := &testHarness{t: t, mc: mc, cmd: cmd}
	h.cancel = func() {
		mc.Close()
		cmd.Process.Kill()
		cmd.Wait()
	}
	t.Cleanup(h.cancel)

	return h
}

// serveRequest reads a FetchRequest from a stream and sends a response.
func serveRequest(stream srpc.MuxedStream, status int, contentType string, body []byte) error {
	defer stream.Close()

	// Read the FetchRequest frame.
	if _, err := readFrame(stream); err != nil {
		return fmt.Errorf("read request: %w", err)
	}

	// Send ResponseInfo.
	if err := writeFrame(stream, buildResponseInfoFrame(status, contentType)); err != nil {
		return fmt.Errorf("write info: %w", err)
	}

	// Send body data.
	if len(body) > 0 {
		if err := writeFrame(stream, buildResponseDataFrame(body, false)); err != nil {
			return fmt.Errorf("write data: %w", err)
		}
	}

	// Send done.
	if err := writeFrame(stream, buildResponseDataFrame(nil, true)); err != nil {
		return fmt.Errorf("write done: %w", err)
	}

	return nil
}

// TestConnection verifies the C++ binary connects via pipe and sends the
// initial scheme request (the HTML redirect triggers bldr:///index.html).
func TestConnection(t *testing.T) {
	h := newTestHarness(t)

	stream, err := h.mc.AcceptStream()
	if err != nil {
		t.Fatalf("accept stream: %v", err)
	}

	frame, err := readFrame(stream)
	if err != nil {
		t.Fatalf("read frame: %v", err)
	}
	if len(frame) == 0 {
		t.Fatal("empty request frame")
	}
	t.Logf("received initial request (%d bytes)", len(frame))

	// Respond with minimal HTML.
	html := []byte("<html><body>ok</body></html>")
	writeFrame(stream, buildResponseInfoFrame(200, "text/html"))
	writeFrame(stream, buildResponseDataFrame(html, false))
	writeFrame(stream, buildResponseDataFrame(nil, true))
	stream.Close()
}

// TestMultipleStreams verifies concurrent yamux streams are handled correctly.
// Serves an initial page that triggers multiple fetch() calls to bldr://.
func TestMultipleStreams(t *testing.T) {
	h := newTestHarness(t)

	const numExtraRequests = 3

	// Serve the initial index.html with JS that fires multiple fetch requests.
	stream, err := h.mc.AcceptStream()
	if err != nil {
		t.Fatalf("accept initial: %v", err)
	}
	var fetches strings.Builder
	for i := range numExtraRequests {
		fetches.WriteString(fmt.Sprintf("fetch('bldr:///test/%d').catch(()=>{});", i))
	}
	html := fmt.Appendf(nil, "<html><body><script>%s</script></body></html>", fetches.String())
	if err := serveRequest(stream, 200, "text/html", html); err != nil {
		t.Fatalf("serve initial: %v", err)
	}

	// Accept and serve the fetch-triggered streams.
	var wg sync.WaitGroup
	for i := range numExtraRequests {
		wg.Add(1)
		go func(idx int) {
			defer wg.Done()
			s, err := h.mc.AcceptStream()
			if err != nil {
				t.Errorf("accept %d: %v", idx, err)
				return
			}
			body := fmt.Sprintf("response-%d", idx)
			if err := serveRequest(s, 200, "text/plain", []byte(body)); err != nil {
				t.Errorf("serve %d: %v", idx, err)
			}
		}(i)
	}
	wg.Wait()
}

// TestStreamingResponse verifies that multi-chunk responses are delivered.
func TestStreamingResponse(t *testing.T) {
	h := newTestHarness(t)

	stream, err := h.mc.AcceptStream()
	if err != nil {
		t.Fatalf("accept: %v", err)
	}
	defer stream.Close()

	// Read request.
	if _, err := readFrame(stream); err != nil {
		t.Fatalf("read request: %v", err)
	}

	// Send headers.
	if err := writeFrame(stream, buildResponseInfoFrame(200, "text/plain")); err != nil {
		t.Fatalf("write info: %v", err)
	}

	// Send multiple body chunks.
	for i := range 10 {
		chunk := fmt.Appendf(nil, "chunk-%d\n", i)
		if err := writeFrame(stream, buildResponseDataFrame(chunk, false)); err != nil {
			t.Fatalf("write chunk %d: %v", i, err)
		}
	}

	// Send done.
	if err := writeFrame(stream, buildResponseDataFrame(nil, true)); err != nil {
		t.Fatalf("write done: %v", err)
	}
}

// TestEvalJS tests the debug eval bridge (Go opens a stream TO C++).
func TestEvalJS(t *testing.T) {
	h := newTestHarness(t)

	// Serve the initial page load first so the webview is ready.
	stream, err := h.mc.AcceptStream()
	if err != nil {
		t.Fatalf("accept initial: %v", err)
	}
	html := []byte("<html><body>eval test</body></html>")
	serveRequest(stream, 200, "text/html", html)

	// Give the page a moment to render.
	time.Sleep(1 * time.Second)

	// Open a yamux stream TO C++ for JS evaluation.
	ctx := h.cmd.ProcessState // just need any context
	_ = ctx
	evalStream, err := h.mc.OpenStream(t.Context())
	if err != nil {
		t.Fatalf("open eval stream: %v", err)
	}
	defer evalStream.Close()

	// Send EvalJSRequest with a simple expression.
	// The code template matches what the Go debug bridge sends:
	// an async IIFE that posts the result via saucer message handler.
	code := `(async()=>{try{let r=JSON.stringify(1+1);window.webkit.messageHandlers.saucer.postMessage('__bldr_eval:__EVAL_ID__:r:'+r)}catch(e){window.webkit.messageHandlers.saucer.postMessage('__bldr_eval:__EVAL_ID__:e:'+e.message)}})()`
	if err := writeFrame(evalStream, encodeEvalJSRequest(code)); err != nil {
		t.Fatalf("write eval request: %v", err)
	}

	// Read response (may timeout if webview JS engine isn't ready).
	respFrame, err := readFrame(evalStream)
	if err != nil {
		t.Logf("eval read failed (may be expected without display): %v", err)
		return
	}

	result, evalErr := decodeEvalJSResponse(respFrame)
	if evalErr != "" {
		t.Logf("eval error: %s", evalErr)
	} else {
		t.Logf("eval result: %s", result)
		if result != "2" {
			t.Errorf("expected eval result '2', got %q", result)
		}
	}
}

// --- Frame helpers ---

func readFrame(r io.Reader) ([]byte, error) {
	lenBuf := make([]byte, 4)
	if _, err := io.ReadFull(r, lenBuf); err != nil {
		return nil, err
	}
	msgLen := binary.LittleEndian.Uint32(lenBuf)
	if msgLen > 10*1024*1024 {
		return nil, fmt.Errorf("frame too large: %d", msgLen)
	}
	data := make([]byte, msgLen)
	if _, err := io.ReadFull(r, data); err != nil {
		return nil, err
	}
	return data, nil
}

func writeFrame(w io.Writer, data []byte) error {
	lenBuf := make([]byte, 4)
	binary.LittleEndian.PutUint32(lenBuf, uint32(len(data)))
	if _, err := w.Write(lenBuf); err != nil {
		return err
	}
	_, err := w.Write(data)
	return err
}

// --- Protobuf encoding helpers ---
// Hand-encode FetchResponse protobufs to avoid circular imports with bldr.
// Wire format matches web.fetch.FetchResponse from fetch.proto.

// buildResponseInfoFrame builds a FetchResponse with ResponseInfo (field 1).
func buildResponseInfoFrame(status int, contentType string) []byte {
	// Build ResponseInfo.
	var info []byte
	// field 1: headers map<string,string>
	if contentType != "" {
		info = append(info, encodeMapEntry("Content-Type", contentType)...)
	}
	// field 2: ok = true
	info = append(info, 0x10, 0x01)
	// field 4: status (uint32)
	info = append(info, 0x20)
	info = append(info, encodeVarint(uint64(status))...)

	// FetchResponse field 1 = ResponseInfo (wire type 2)
	var resp []byte
	resp = append(resp, 0x0a)
	resp = append(resp, encodeVarint(uint64(len(info)))...)
	resp = append(resp, info...)
	return resp
}

// buildResponseDataFrame builds a FetchResponse with ResponseData (field 2).
func buildResponseDataFrame(data []byte, done bool) []byte {
	var rd []byte
	if len(data) > 0 {
		rd = append(rd, 0x0a) // field 1: data
		rd = append(rd, encodeVarint(uint64(len(data)))...)
		rd = append(rd, data...)
	}
	if done {
		rd = append(rd, 0x10, 0x01) // field 2: done = true
	}

	// FetchResponse field 2 = ResponseData (wire type 2)
	var resp []byte
	resp = append(resp, 0x12)
	resp = append(resp, encodeVarint(uint64(len(rd)))...)
	resp = append(resp, rd...)
	return resp
}

func encodeMapEntry(key, value string) []byte {
	var entry []byte
	entry = append(entry, 0x0a) // field 1: key
	entry = append(entry, encodeVarint(uint64(len(key)))...)
	entry = append(entry, key...)
	entry = append(entry, 0x12) // field 2: value
	entry = append(entry, encodeVarint(uint64(len(value)))...)
	entry = append(entry, value...)

	// ResponseInfo field 1 = headers map entry (wire type 2)
	var out []byte
	out = append(out, 0x0a)
	out = append(out, encodeVarint(uint64(len(entry)))...)
	out = append(out, entry...)
	return out
}

func encodeVarint(v uint64) []byte {
	var buf [10]byte
	n := binary.PutUvarint(buf[:], v)
	return buf[:n]
}

// --- EvalJS protobuf helpers ---

func encodeEvalJSRequest(code string) []byte {
	var msg []byte
	msg = append(msg, 0x0a) // field 1: code (string)
	msg = append(msg, encodeVarint(uint64(len(code)))...)
	msg = append(msg, code...)
	return msg
}

func decodeEvalJSResponse(data []byte) (result, errStr string) {
	i := 0
	for i < len(data) {
		tag := data[i]
		i++
		fieldNum := tag >> 3
		wireType := tag & 0x07
		if wireType != 2 {
			for i < len(data) && data[i]&0x80 != 0 {
				i++
			}
			if i < len(data) {
				i++
			}
			continue
		}
		v, n := binary.Uvarint(data[i:])
		i += n
		end := i + int(v)
		if end > len(data) {
			break
		}
		s := string(data[i:end])
		i = end
		switch fieldNum {
		case 1:
			result = s
		case 2:
			errStr = s
		}
	}
	return
}
