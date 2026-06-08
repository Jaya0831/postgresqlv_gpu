-- Calculate derived metrics
SELECT 
    '=== BOTTLENECK ANALYSIS ===' as analysis;

SELECT 
    'Lock Contention Ratio' as metric,
    CASE 
        WHEN lock_acquire_count > 0 
        THEN ROUND((lock_wait_count::numeric * 100.0 / lock_acquire_count), 2)
        ELSE 0 
    END as value,
    CASE 
        WHEN lock_acquire_count > 0 AND (lock_wait_count::numeric * 100.0 / lock_acquire_count) > 10
        THEN 'HIGH - Ring buffer lock is a bottleneck!'
        WHEN lock_acquire_count > 0 AND (lock_wait_count::numeric * 100.0 / lock_acquire_count) > 5
        THEN 'MEDIUM - Some lock contention'
        ELSE 'LOW - Lock contention is not a major issue'
    END as interpretation
FROM (
    SELECT 
        MAX(CASE WHEN metric_name = 'lock_acquire_count' THEN metric_value ELSE 0 END) as lock_acquire_count,
        MAX(CASE WHEN metric_name = 'lock_wait_count' THEN metric_value ELSE 0 END) as lock_wait_count
    FROM pg_vector_search_profiling_stats()
) subq;

SELECT 
    'Worker Utilization' as metric,
    CASE 
        WHEN (total_processing + total_idle) > 0
        THEN ROUND((total_processing::numeric * 100.0 / (total_processing + total_idle)), 2)
        ELSE 0
    END as value,
    CASE 
        WHEN (total_processing + total_idle) > 0 AND (total_processing::numeric * 100.0 / (total_processing + total_idle)) > 80
        THEN 'HIGH - Worker is busy, may be bottleneck'
        WHEN (total_processing + total_idle) > 0 AND (total_processing::numeric * 100.0 / (total_processing + total_idle)) > 50
        THEN 'MEDIUM - Worker has moderate load'
        ELSE 'LOW - Worker has idle time'
    END as interpretation
FROM (
    SELECT 
        MAX(CASE WHEN metric_name = 'total_worker_processing_time_us' THEN metric_value ELSE 0 END) as total_processing,
        MAX(CASE WHEN metric_name = 'total_worker_idle_time_us' THEN metric_value ELSE 0 END) as total_idle
    FROM pg_vector_search_profiling_stats()
) subq;

SELECT 
    'Queue Health' as metric,
    MAX(CASE WHEN metric_name = 'queue_full_count' THEN metric_value ELSE 0 END) as queue_full_count,
    MAX(CASE WHEN metric_name = 'max_queue_depth' THEN metric_value ELSE 0 END) as max_queue_depth,
    CASE 
        WHEN MAX(CASE WHEN metric_name = 'queue_full_count' THEN metric_value ELSE 0 END) > 0
        THEN 'WORKER BOTTLENECK - Queue filled up!'
        WHEN MAX(CASE WHEN metric_name = 'max_queue_depth' THEN metric_value ELSE 0 END) > 10
        THEN 'POTENTIAL WORKER BOTTLENECK - High queue depth'
        ELSE 'OK - Queue stays relatively empty'
    END as interpretation
FROM pg_vector_search_profiling_stats();

SELECT 
    '=== KEY METRICS SUMMARY ===' as summary;

SELECT 
    metric_name,
    metric_value,
    description
FROM pg_vector_search_profiling_stats()
WHERE metric_name IN (
    'lock_contention_ratio_percent',
    'queue_full_count',
    'max_queue_depth',
    'worker_utilization_percent',
    'avg_worker_processing_time_us',
    'avg_result_wait_time_us',
    'avg_lock_wait_time_us'
)
ORDER BY metric_name;
