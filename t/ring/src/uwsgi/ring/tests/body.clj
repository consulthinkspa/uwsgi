(ns uwsgi.ring.tests.body
  (:use [compojure.core]))

; generating primary numbers
; http://clojuredocs.org/clojure_core/clojure.core/lazy-seq#example_1000
(defn sieve [s]
  (cons (first s)
        (lazy-seq (sieve (filter #(not= 0 (mod % (first s)))
                                 (rest s))))))

(defn sequence [] (take 20 (sieve (iterate inc 2))))

(defn file [] (java.io.File. "CONTRIBUTORS"))

(defn stream [] (java.io.FileInputStream. (java.io.File. "CONTRIBUTORS")))

(defroutes app-routes
  (GET "/sequence" [] (sequence))
  (GET "/file" [] (file))
  (GET "/stream" [] (stream)))