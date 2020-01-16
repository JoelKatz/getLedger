# getLedger
Retrieve an XRP Ledger in JSON by sequence number

Run the "build" script. Edit it if necessary.

It will produce two executables:
ledger.compact will get ledgers in compact JSON
ledger.pretty will get ledgers in pretty JSON

Both programs take a single parameter, the ledger index
to be retrieved.

They will produce a file called ledger.<number> which contains
the ledger header, transactions (if any), and full state tree.

This is quick and dirty code that has not been well-tested
or thoroughly debugged. In particular, it does not handle
error conditions well.
