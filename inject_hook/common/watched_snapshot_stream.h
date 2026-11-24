#pragma once

#include "state_snapshot_child.h"
#include "watched_snapshot.h"
#include "watched_identity_wire.h"
#include <cstdio>

// Prototype protocol: both ends must be supervised and bounded. A dirty frame
// invalidates every older candidate; a later frame can carry a fresh state.
// A live stream is essential: a snapshot frame alone cannot prove that no
// change occurred after it was written or before the UI applies it.
namespace WatchedSnapshotStream
{
const DWORD kMagic = 0x3157534c; // LSW1
enum class Kind : DWORD { Ready = 1, Dirty = 2, Snapshot = 3, Error = 4, Heartbeat = 5, Refresh = 6 };
struct Header { DWORD magic, kind, status, length; unsigned long long generation; };
static_assert(sizeof(Header) == 24, "unexpected watcher stream layout");

// A receiver is scoped to one child and one target. It must be used on a
// single thread; the owner serializes delivery with candidate application.
// Transport failure or a missed heartbeat must separately call Fail().
class Receiver
{
public:
	bool Accept(const Header& header, const std::vector<BYTE>& body)
	{
		if( failed_ ) return false;
		if( header.magic != kMagic || header.length != body.size()
			|| body.size() > SnapshotWire::kMaxBytes ) return Fail(ERROR_INVALID_DATA);
		const Kind kind = static_cast<Kind>(header.kind);
		if( kind == Kind::Error )
		{
			if( !header.status || header.length ) return Fail(ERROR_INVALID_DATA);
			return Fail(header.status);
		}
		if( header.status || !header.generation ) return Fail(ERROR_INVALID_DATA);
		if( kind == Kind::Ready )
		{
			if( ready_ || header.length ) return Fail(ERROR_INVALID_DATA);
			ready_ = true; dirty_ = true; generation_ = header.generation; return true;
		}
		if( !ready_ ) return Fail(ERROR_INVALID_DATA);
		if( kind == Kind::Refresh )
		{
			if( header.length != sizeof(std::uint64_t) || header.generation <= generation_ ) return Fail(ERROR_INVALID_DATA);
			std::uint64_t request = 0; memcpy(&request, body.data(), sizeof(request));
			if( !request || request <= refreshRequest_ ) return Fail(ERROR_INVALID_DATA);
			refreshRequest_ = request; generation_ = header.generation; dirty_ = true;
			candidate_ = WatchedSnapshot::Candidate(); return true;
		}
		if( kind == Kind::Dirty )
		{
			if( header.length || header.generation <= generation_ ) return Fail(ERROR_INVALID_DATA);
			generation_ = header.generation; dirty_ = true; candidate_ = WatchedSnapshot::Candidate(); return true;
		}
		if( kind == Kind::Heartbeat )
		{
			if( header.length || header.generation != generation_ ) return Fail(ERROR_INVALID_DATA);
			return true;
		}
		if( kind == Kind::Snapshot )
		{
			if( !dirty_ || header.generation != generation_ || !header.length ) return Fail(ERROR_INVALID_DATA);
			try
			{
				DirectorySnapshot::State state;
				DirectoryEntryIdentity::State identities; bool hasIdentities = false;
				if( !WatchedIdentityWire::Decode(body, state, identities, hasIdentities) ) return Fail(ERROR_INVALID_DATA);
				candidate_.generation = generation_; candidate_.state = std::move(state);
				candidate_.hasEntryIdentities = hasIdentities; candidate_.entryIdentities = std::move(identities);
				dirty_ = false; return true;
			}
			catch( const std::bad_alloc& ) { return Fail(ERROR_NOT_ENOUGH_MEMORY); }
			catch( ... ) { return Fail(ERROR_GEN_FAILURE); }
		}
		return Fail(ERROR_INVALID_DATA);
	}
	bool Fail(DWORD error)
	{
		failed_ = true; dirty_ = true; candidate_ = WatchedSnapshot::Candidate();
		error_ = error ? error : ERROR_GEN_FAILURE; return false;
	}
	bool Current(std::uint64_t generation) const
	{ return ready_ && !failed_ && !dirty_ && candidate_.generation == generation && generation_ == generation; }
	const WatchedSnapshot::Candidate& StateCandidate() const { return candidate_; }
	DirectorySnapshot::State ReleaseState() { return std::move(candidate_.state); }
	WatchedSnapshot::Candidate ReleaseCandidate() { return std::move(candidate_); }
	DWORD Error() const { return error_; }
	std::uint64_t RefreshRequest() const { return refreshRequest_; }
private:
	WatchedSnapshot::Candidate candidate_ = {};
	std::uint64_t generation_ = 0, refreshRequest_ = 0;
	DWORD error_ = ERROR_SUCCESS;
	bool ready_ = false, dirty_ = true, failed_ = false;
};

inline bool WriteFrame(HANDLE pipe, Kind kind, DWORD status, std::uint64_t generation,
	const std::vector<BYTE>* body = nullptr)
{
	const size_t length = body ? body->size() : 0;
	if( length > SnapshotWire::kMaxBytes ) return false;
	const Header header = { kMagic, static_cast<DWORD>(kind), status,
		static_cast<DWORD>(length), generation };
	return SnapshotWire::WriteAll(pipe, &header, sizeof(header))
		&& (!length || SnapshotWire::WriteAll(pipe, body->data(), length));
}

#if UNICODEHACK_SWEEP_DIAGNOSTICS
inline void TraceRead(bool success, DWORD error, std::uint64_t generation, size_t count, unsigned stage, ULONGLONG elapsed)
{
	wchar_t path[MAX_PATH * 4] = {};
	const DWORD length = ::GetEnvironmentVariableW(L"LEYEES_STATE_WATCH_TRACE_FILE", path, _countof(path));
	if( !length || length >= _countof(path) ) return;
	DirectorySnapshot::Handle file(::CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
	if( file.value == INVALID_HANDLE_VALUE ) return;
	char line[160] = {};
	const int size = sprintf_s(line, "tick=%llu success=%d error=%lu generation=%llu count=%llu stage=%u elapsed_ms=%llu\r\n",
		static_cast<unsigned long long>(::GetTickCount64()), success ? 1 : 0, static_cast<unsigned long>(error),
		static_cast<unsigned long long>(generation), static_cast<unsigned long long>(count), stage,
		static_cast<unsigned long long>(elapsed));
	if( size > 0 ) { DWORD written = 0; ::WriteFile(file.value, line, static_cast<DWORD>(size), &written, nullptr); }
}
#endif

// This function may block on filesystem or pipe operations; execute it in a
// process that the owner can terminate and whose exit it can verify.
inline int RunChild(HANDLE pipe, const wchar_t* path, HANDLE control = nullptr, bool collectEntryIdentities = false)
{
	if( !pipe || pipe == INVALID_HANDLE_VALUE || ::GetFileType(pipe) != FILE_TYPE_PIPE || !path || !*path ) return 2;
	DWORD error = ERROR_SUCCESS;
	try
	{
		WatchedSnapshot watch;
		if( !watch.Open(path, error, collectEntryIdentities) )
		{ WriteFrame(pipe, Kind::Error, error ? error : ERROR_GEN_FAILURE, 0); return 3; }
		std::uint64_t announced = watch.Generation();
		if( !WriteFrame(pipe, Kind::Ready, 0, announced) ) return 4;
		bool dirty = true;
		std::uint64_t lastRequest = 0;
		ULONGLONG nextRead = ::GetTickCount64(), nextHeartbeat = nextRead + 1000;
		for( ;; )
		{
			if( control )
			{
				DWORD available = 0;
				if( !::PeekNamedPipe(control, nullptr, 0, nullptr, &available, nullptr) ) return 4;
				if( available >= sizeof(std::uint64_t) )
				{
					std::uint64_t request = 0; DWORD received = 0;
					if( !::ReadFile(control, &request, sizeof(request), &received, nullptr)
						|| received != sizeof(request) || !request || request <= lastRequest ) return 4;
					lastRequest = request;
					#if UNICODEHACK_SWEEP_DIAGNOSTICS
					// Diagnostic only: allow a real filesystem mutation between an
					// application and the subsequent verification request.
					if( request == 2 && ::GetEnvironmentVariableW(L"LEYEES_LIST_WATCH_TEST_VERIFY_STALL", nullptr, 0) ) ::Sleep(INFINITE);
					if( request == 2 && ::GetEnvironmentVariableW(L"LEYEES_LIST_WATCH_TEST_VERIFY_DELAY", nullptr, 0) ) ::Sleep(1200);
					#endif
					if( !watch.RequestRefresh(error) ) { WriteFrame(pipe, Kind::Error, error, watch.Generation()); return 3; }
					announced = watch.Generation();
					std::vector<BYTE> body(sizeof(request)); memcpy(body.data(), &request, sizeof(request));
					if( !WriteFrame(pipe, Kind::Refresh, 0, announced, &body) ) return 4;
					dirty = true; nextRead = 0;
				}
			}
			const ULONGLONG now = ::GetTickCount64();
			if( dirty && now >= nextRead )
			{
				WatchedSnapshot::Candidate candidate;
				#if UNICODEHACK_SWEEP_DIAGNOSTICS
				const ULONGLONG readStarted = ::GetTickCount64();
				#endif
				const bool read = watch.Read(candidate, error);
				#if UNICODEHACK_SWEEP_DIAGNOSTICS
				TraceRead(read, error, watch.Generation(), candidate.state.entries.size(), watch.LastReadStage(), ::GetTickCount64() - readStarted);
				#endif
				// Avoid phase-locking every read to a producer that writes at an
				// exact divisor of the nominal interval. This is a bounded retry
				// variation, not a guarantee of intermediate snapshots.
				nextRead = ::GetTickCount64() + 287 + watch.Generation() % 41;
				if( watch.Generation() != announced )
				{
					announced = watch.Generation();
					if( !WriteFrame(pipe, Kind::Dirty, 0, announced) ) return 4;
				}
				if( read )
				{
					std::vector<BYTE> body;
					if( !(candidate.hasEntryIdentities
						? WatchedIdentityWire::Encode(candidate.state, candidate.entryIdentities, body)
						: SnapshotWire::Encode(candidate.state, body)) )
					{ WriteFrame(pipe, Kind::Error, ERROR_BUFFER_OVERFLOW, announced); return 3; }
					if( !WriteFrame(pipe, Kind::Snapshot, 0, candidate.generation, &body) ) return 4;
					dirty = false;
				}
				else if( error != ERROR_RETRY )
				{ WriteFrame(pipe, Kind::Error, error ? error : ERROR_GEN_FAILURE, announced); return 3; }
			}
			bool changed = false;
			const ULONGLONG beforeWait = ::GetTickCount64();
			DWORD waitMs = 100;
			if( dirty ) waitMs = nextRead <= beforeWait ? 0 : static_cast<DWORD>((std::min)(100ULL, nextRead - beforeWait));
			if( !watch.Poll(waitMs, changed, error) )
			{ WriteFrame(pipe, Kind::Error, error ? error : ERROR_GEN_FAILURE, watch.Generation()); return 3; }
			if( changed )
			{
				#if UNICODEHACK_SWEEP_DIAGNOSTICS
				if( ::GetEnvironmentVariableW(L"LEYEES_LIST_WATCH_TEST_RESTART", nullptr, 0)
					|| ::GetEnvironmentVariableW(L"LEYEES_LIST_WATCH_TEST_DELAY_DIRTY", nullptr, 0) )
				{
					const std::wstring probe = std::wstring(path) + L"\\__late_watch_*.webp";
					WIN32_FIND_DATAW data = {}; HANDLE found = ::FindFirstFileW(probe.c_str(), &data);
					if( found != INVALID_HANDLE_VALUE ) { ::FindClose(found); ::Sleep(1000); }
				}
				#endif
				// Once the receiver is already dirty, further completions only
				// advance the internal generation. Announce the latest generation
				// before the next snapshot instead of streaming every completion.
				if( !dirty )
				{
					announced = watch.Generation();
					if( !WriteFrame(pipe, Kind::Dirty, 0, announced) ) return 4;
				}
				dirty = true;
			}
			const ULONGLONG tick = ::GetTickCount64();
			if( tick >= nextHeartbeat )
			{
				if( !WriteFrame(pipe, Kind::Heartbeat, 0, announced) ) return 4;
				nextHeartbeat = tick + 1000;
			}
		}
	}
	catch( const std::bad_alloc& ) { WriteFrame(pipe, Kind::Error, ERROR_NOT_ENOUGH_MEMORY, 0); }
	catch( ... ) { WriteFrame(pipe, Kind::Error, ERROR_GEN_FAILURE, 0); }
	return 3;
}
}
