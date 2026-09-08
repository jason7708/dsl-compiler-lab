struct QuoteEvent {
    double bid;
    double ask;
};
struct TradeEvent {
    double price;
    bool valid;
};
struct TradeError {
    int code;
};

struct PriceTracker {
    double mid = 0.0;
    double last_trade = 0.0;

    double operator()(QuoteEvent event) {
        mid = (event.bid + event.ask) / 2.0;
        return mid;
    }

    bool operator()(TradeEvent event) {
        last_trade = event.price;
        if (!event.valid)
            throw TradeError{1};
        return last_trade > mid;
    }
};
