#pragma once
//
// line_tee_stream.hpp
//
// An std::ostream that forwards every character it receives to an
// underlying destination streambuf (e.g. a std::ofstream's rdbuf()),
// while also letting you inspect the content line-by-line via a callback.
//
// Because it's implemented as a std::streambuf, it works transparently
// with the whole ostream machinery: operator<<, std::endl, manipulators,
// formatted output, etc. -- you don't lose any of that.
//
// Initial version generated via Claude Sonnet 5 (Medium), 2026-08-22

#include <streambuf>
#include <ostream>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------
// LineTeeStreambuf
// ---------------------------------------------------------------------
class LineTeeStreambuf : public std::streambuf {
public:
    using LineCallback = std::function<void(const std::string& line)>;

    // dest: the streambuf everything gets forwarded to (not owned).
    // cb:   called once per completed line (without the trailing '\n').
    explicit LineTeeStreambuf(std::streambuf* dest, LineCallback cb = nullptr)
        : dest_(dest), callback_(std::move(cb)) {}

    void setLineCallback(LineCallback cb) { callback_ = std::move(cb); }

    // Access to whatever's been written since the last '\n' (partial line).
    const std::string& pendingPartialLine() const { return currentLine_; }

    // If true (default false), a trailing '\r' before '\n' is stripped
    // before the line is handed to the callback (handles CRLF input).
    void stripCarriageReturn(bool strip) { stripCR_ = strip; }

protected:
    // Called when the internal buffer is "full" -- since we don't use
    // an internal buffer (unbuffered mode), this is called per character
    // whenever put-area logic falls through. We funnel single chars here.
    int_type overflow(int_type ch) override {
        if (traits_type::eq_int_type(ch, traits_type::eof()))
            return traits_type::not_eof(ch);
        handleChar(traits_type::to_char_type(ch));
        return ch;
    }

    // Bulk writes (operator<<(const char*) etc. often land here) --
    // handle char-by-char so line splitting stays correct.
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        for (std::streamsize i = 0; i < n; ++i)
            handleChar(s[i]);
        return n;
    }

    int sync() override {
        return dest_ && dest_->pubsync() == 0 ? 0 : -1;
    }

private:
    void handleChar(char c) {
        if (dest_) dest_->sputc(c); // forward immediately to the real target

        if (c == '\n') {
            if (stripCR_ && !currentLine_.empty() && currentLine_.back() == '\r')
                currentLine_.pop_back();
            if (callback_) callback_(currentLine_);
            currentLine_.clear();
        } else {
            currentLine_.push_back(c);
        }
    }

    std::streambuf* dest_;
    std::string currentLine_;
    LineCallback callback_;
    bool stripCR_ = false;
};

// ---------------------------------------------------------------------
// LineTeeStream: the ostream wrapper around LineTeeStreambuf
// ---------------------------------------------------------------------
class LineTeeStream : public std::ostream {
public:
    explicit LineTeeStream(std::streambuf* dest,
                            LineTeeStreambuf::LineCallback cb = nullptr)
        : std::ostream(&buf_), buf_(dest, std::move(cb)) {}

    void setLineCallback(LineTeeStreambuf::LineCallback cb) {
        buf_.setLineCallback(std::move(cb));
    }
    void stripCarriageReturn(bool strip) { buf_.stripCarriageReturn(strip); }
    const std::string& pendingPartialLine() const { return buf_.pendingPartialLine(); }

private:
    LineTeeStreambuf buf_;
};

// ---------------------------------------------------------------------
// Convenience: owns the file itself, in case you don't want to manage
// an std::ofstream separately.
// ---------------------------------------------------------------------
class LineTeeFileStream : public std::ostream {
public:
    explicit LineTeeFileStream(const std::string& path,
                                LineTeeStreambuf::LineCallback cb = nullptr,
                                std::ios::openmode mode = std::ios::out)
        : std::ostream(&buf_), file_(path, mode), buf_(file_.rdbuf(), std::move(cb)) {}

    void setLineCallback(LineTeeStreambuf::LineCallback cb) {
        buf_.setLineCallback(std::move(cb));
    }
    bool isOpen() const { return file_.is_open(); }

private:
    std::ofstream file_;
    LineTeeStreambuf buf_;
};

// ---------------------------------------------------------------------
// Alternative: instead of a callback, just collect all lines so you can
// inspect them later (e.g. for tests). Handy if you don't want the
// inspection logic interleaved with writing.
// ---------------------------------------------------------------------
class LineCollector {
public:
    void operator()(const std::string& line) { lines_.push_back(line); }
    const std::vector<std::string>& lines() const { return lines_; }
    void clear() { lines_.clear(); }

private:
    std::vector<std::string> lines_;
};

/* ---------------------------------------------------------------------
Example usage:

#include "line_tee_stream.hpp"
#include <iostream>

int main() {
    std::ofstream file("output.log");

    // Option 1: callback fired per line
    LineTeeStream tee(file.rdbuf(), [](const std::string& line) {
        std::cout << "[captured] " << line << "\n";
    });

    tee << "Hello, world!" << std::endl;
    tee << "value = " << 42 << std::endl;
    tee << "no newline yet";       // not visible to callback yet
    tee << " ...done" << std::endl; // now callback fires with full line

    // Option 2: collect lines for later inspection
    LineCollector collector;
    LineTeeFileStream tee2("output2.log", std::ref(collector));
    tee2 << "line one\nline two\n";
    for (auto& l : collector.lines()) std::cout << l << "\n";
}
--------------------------------------------------------------------- */
