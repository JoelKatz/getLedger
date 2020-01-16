#include <string.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <curl/curl.h>
#include <json/json.h>

// This is very quick and dirty code to retrieve a ledger
// by sequence number from the S2 full history cluster

char *out_buf;

bool pretty = true;
std::string space = " ";
std::string eol = "\n";
std::string indent = "\t";

// Switch from pretty-printing to compact JSON
void setCompact()
{
    pretty = false;
    space = "";
    eol = "";
    indent = "";
}

// Structure to pass to the CURL write callback
struct user_data
{
    char *buf;
    int so_far;
};

// The CURL code needs this callback
size_t write_callback (char *ptr, size_t size, size_t len, void *userdata)
{
    struct user_data* ud = (struct user_data*) userdata;
    memcpy(ud->buf + ud->so_far, ptr, size * len);
    ud->so_far += size * len;
    return size * len;
}

// Write a JSON object to a stream
void writeJson(std::ofstream& f, int step, Json::Value const &v)
{
    std::string pre;
    std::string str;

    if (! pretty)
    {
        Json::FastWriter w;
        str = w.write (v);
    }
    else
    {
        for (int i = 0; i < step; ++i)
            pre += indent;
        Json::StyledStreamWriter w (indent);
        std::ostringstream out;
        w.write (out, v);
        std::istringstream in(out.str());
        str = out.str();
    }

    // Read the output line-by-line, apply
    // formatting as needed, and write to output
    std::string l;
    bool first = true;
    std::istringstream in (str);
    while (std::getline (in, l))
    {
        if ((l == indent) || (l == ""))
            continue;
        if (first)
            first = false;
        else
            f << eol;
        f << pre << l;
    }
}

// Execute a query against the S2 cluster of full history XRP Ledger notes.
// Note that this is a best-effort service that does not guarantee
// any particular level of reliability.
bool do_query (std::string const& method, Json::Value const& params, Json::Value& reply)
{
    std::string q;

    {
        Json::Value query = Json::objectValue;
        query["method"] = method;
        Json::Value& p = (query["params"] = Json::arrayValue);
        p.append(params);

        Json::FastWriter w;
        q = w.write (query);
    }

    CURL* c = curl_easy_init();
    if (c == NULL)
        return false;

    struct user_data ud;
    ud.buf = out_buf;
    ud.so_far = 0;

    CURLcode code;
    code = curl_easy_setopt (c, CURLOPT_URL, "http://s2.ripple.com:51234");
    code = curl_easy_setopt (c, CURLOPT_POSTFIELDSIZE, q.size());
    code = curl_easy_setopt (c, CURLOPT_POSTFIELDS, q.c_str());
    code = curl_easy_setopt (c, CURLOPT_WRITEFUNCTION, write_callback);
    code = curl_easy_setopt (c, CURLOPT_WRITEDATA, &ud);

    code = curl_easy_perform (c);
    if (code != CURLE_OK)
    {
        curl_easy_cleanup (c);
        return false;
    }
    ud.buf[ud.so_far] = 0;
    curl_easy_cleanup (c);

    Json::Reader reader;
    Json::Value root;
    if (!reader.parse(out_buf, root))
    {
        fprintf(stderr, "%s\n", reader.getFormatedErrorMessages().c_str());
        return false;
    }
    Json::Value& result = root["result"];
    if (! result.isObject())
    {
        fprintf(stderr, "Result is not object\n");
        return false;
    }
    Json::Value& status = result["status"];
    if (!status.isString() || (status.asString() != "success"))
    {
        fprintf(stderr, "Result is '%s', not success\n", status.asString().c_str());
        reply = result;
        return false;
    }

    reply = std::move (root);
    return true;
}

// Get the header of a ledger given its sequence number
bool getHeader (unsigned ledger_seq, Json::Value& header)
{
    Json::Value reply;
    Json::Value params = Json::objectValue;
    params["ledger_index"] = ledger_seq;
    if (! do_query ("ledger", params, reply))
    {
        header = reply;
        return false;
    }
    header = reply["result"]["ledger"];
    if (header.isObject() && !header.isNull())
        return true;
    header = reply;
    return false;
}

// Get the transactions from a ledger given its sequence number
// (with metadata)
bool getTxns (unsigned ledger_seq, Json::Value& txns)
{
    Json::Value reply;
    Json::Value params = Json::objectValue;
    params["ledger_index"] = ledger_seq;
    params["transactions"] = true;
    params["expand"] = true;
    if (! do_query ("ledger", params, reply))
        return false;
    txns = std::move (reply["result"]["ledger"]);
    if (txns.isNull() || !txns.isObject())
        return false;
    txns = std::move (txns["transactions"]);
    return txns.isArray() && !txns.isNull();
}

// Get a chunk of a ledger's state tree
bool getState (unsigned ledger_seq, Json::Value& entries, Json::Value& marker)
{
    Json::Value reply;
    Json::Value params = Json::objectValue;
    params["ledger_index"] = ledger_seq;
    params["binary"] = false;
    if (! marker.isNull())
        params["marker"] = marker;
    if (! do_query ("ledger_data", params, reply))
        return false;
    reply = std::move (reply["result"]);
    marker = reply["marker"];
    entries = std::move(reply["state"]);
    return true;
}

// Old school hacky progress indicator
// Sorry, not sorry
void do_progress(int max, Json::Value const& marker)
{
    static int so_far = 0;
    static int phase = 0;
    char phases[4] = { '\\', '|', '/', '-' };

    int progress = max;
    if (marker.isString())
    {
        // Hashes are randomly distributed, so we can tell how much
        // progress we made by where we are in the hash space.
        // This code only looks at the first two characters, 00-FF.
        std::string m = marker.asString();
        progress = 0;
        if (m[0] >= '0' && m[0] <= '9') progress += 16 * (m[0] - '0');
        if (m[0] >= 'A' && m[0] <= 'Z') progress += 16 * (m[0] + 10 - 'A');
        if (m[1] >= '0' && m[1] <= '9') progress += m[1] - '0';
        if (m[1] >= 'A' && m[1] <= 'Z') progress += m[1] + 10 - 'A';
        progress *= max;
        progress /= 255;
    }
    std::cout << "\x8 \x8";
    while (so_far < progress)
    {
        std::cout << "*";
        ++so_far;
    }
    std::cout << phases[++phase % 4] << std::flush;
}

int main(int argc, char* argv[])
{
    if (argc != 2)
    {
        fprintf (stderr, "You must specify a ledger by index\n");
        return -1;
    }

    // When compiling, define "COMPACT" if you want your JSON
    // to be as short as possible instead of being neatly
    // spaced and indented. 
#ifdef COMPACT
    setCompact();
#endif

    out_buf = (char *) malloc (100000000);
    if (out_buf == NULL)
        return -1;

    unsigned seq = (unsigned) atoll(argv[1]);
    printf("Seq = %u\n", seq);

    char fbuf[512];
    sprintf(fbuf, "ledger.%u", seq);
    std::ofstream f (fbuf, std::ofstream::out | std::ofstream::trunc);
    if (! f.is_open())
        return -1;

    f << "{" << eol;

    Json::Value reply;
    if (! getHeader(seq, reply))
    {
        fprintf(stderr, "Query failed");
        std::cerr << reply;
        return -1;
    }
    f << indent << "\"ledger\"" << space << ":" << eol;;
    writeJson (f, 1, reply);
    f << "," << eol;

    printf("Got header\n");

    // If transaction_hash is all zeroes, there are no transactions to get
    if (reply["transaction_hash"].asString() !=
        "0000000000000000000000000000000000000000000000000000000000000000")
    {
        if (! getTxns(seq, reply))
        {
            fprintf(stderr, "Query failed\n");
            return -1;
        }
        f << indent << "\"transactions\":" << eol;
        writeJson (f, 1, reply);
        f << indent << "," << eol;;
        printf("Got %d transaction(s)\n", reply.size());
    }
    else printf("No transactions\n");
    printf("Getting state tree:\n");
    int e = 0;

    std::string const progress = "0%--10%--20%--30%--40%--50%--60%--70%--80%--90%--100%";
    std::cout << progress << std::endl;

    bool first = true;
    f << indent << "\"state\"" << space << ":" << eol << indent << "[" << eol;
    Json::Value marker = Json::nullValue;
    do
    {
        if (! getState (seq, reply, marker))
        {
            fprintf(stderr, "Query failed\n");
            return -1;
        }
        for (unsigned j = 0u; j < reply.size(); ++j)
        {
            // We need to write out each state entry
            // separately so we don't get start of object
            // and end of object stuff from the chunk
            if (first)
                first = false;
            else
                f << "," << eol;
            writeJson (f, 2, reply[j]);
            ++e;
        }
        do_progress (progress.size(), marker);
        // empty returned marker indicates last chunk of data
    } while (!  marker.isNull());

    std::cout << "\x8 " << std::endl;
    printf("%d state entries\n", e);

    f << eol << indent << "]" << eol;
    f << "}" << eol;
    f.close();
    printf("File \"%s\" written\n", fbuf);

    return 0;
}
