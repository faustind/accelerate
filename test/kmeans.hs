--
-- K-Means sample. This uses the CUDA backend to execute the program.
--
-- Run the generate-samples program first to create some random data.
--

import Prelude                                          as P
import TestCore
import Data.Binary (decodeFile)
import Criterion.Main
import System.Mem

import Data.Array.Accelerate                            as A
import Data.Array.Accelerate.CUDA                       as CUDA
import Data.Array.Accelerate.Math.Kmeans


main :: IO ()
main = do
  points'   <- decodeFile "points.bin"          :: IO [Point]
  clusters' <- read `fmap` readFile "clusters"  :: IO [Cluster]
  let nclusters = length clusters'
      npoints   = length points'

      clusters  = A.fromList (Z:.nclusters) clusters'
      points    = A.fromList (Z:.npoints)   points'

      step      = CUDA.run1 (kmeans nclusters (use points))
      final     = step clusters

  clusters `seq` points `seq` performGC

  putStrLn $ "number of points: " P.++ show npoints
  putStrLn $ "final clusters:\n" P.++ unlines (P.map show (A.toList final))

  defaultMain [ bench "kmeans/cuda" $ whnf step clusters ]

