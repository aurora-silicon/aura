The producer must withhold the descriptor on wrong chip, board, model, ADT mapping,
reset GPIO/function, or DART geometry before training. Any already-active DART,
busy TLB or endpoint bus master is an existing owner and must not be reset.
After training, absent/mismatched identities, any BME, invalid BAR sizing,
dropped BAR/bridge/command write, live ROM or changed DART state must hold PERST
and prove link/port drain before returning a clean failure. A failed clamp must
return a distinct quarantined failure. No failure may leave a valid descriptor.
Repeated successful calls must not retrain an owned device. The descriptor must
have exactly the Mu v1 ABI, with six fixed resource subranges.
