#!/usr/bin/env Rscript --vanilla


load_data <- function(file) {
  r <- read.table(file,sep=',',head=F,comment.char='#',as.is=T)
  return(r)
}

hists <- list()
hists[[1]] <- load_data('hists/bubbles.hist.csv')
hists[[2]] <- load_data('hists/breakpoints.hist.csv')
hists[[3]] <- load_data('hists/platypus.hist.csv')
hists[[4]] <- load_data('hists/freebayes.hist.csv')
hists[[5]] <- load_data('hists/cortex.hist.csv')

names <- c('McCortex Bubbles', 'McCortex Breakpoints', 'Platypus','Freebayes','Cortex')
# colours <- c('red', 'green', 'blue', 'black', 'purple')

library(RColorBrewer)
line_cols <- brewer.pal(5, "Pastel1")

colours <- rep('black', 5)

# points <- c('o','o','o', 'o', 'o')
# points <- rep(3, 5)
# points <- c(4, 4, 4, 4, 4)
# points <- c(1, 2, 3, 20, 18)
# points <- rep('*', 5)
# points <- c(0, 1, 2, 3, 4)
points <- c(16, 17, 0, 1, 6)

title <- expression(paste(italic('Klebsiella pneumoniae'),' indels'))

ncalls <- length(hists)
lim <- 100
m <- matrix(0, nrow=2*lim+1, ncol=ncalls+1)
m[,1] <- -lim:lim
colnames(m) <- c("dist",names)
x <- -lim:lim

# Merge
for(i in 1:length(hists)) {
  l <- hists[[i]]
  for(j in 1:nrow(l)) {
    d <- l[j,1]
    d <- min(d, lim)
    d <- max(d,-lim)
    d <- d + lim + 1
    m[d,1+i] <- m[d,1+i] + l[j,2]
  }
}

# Map 0->1
for(i in 1:nrow(m)) {
  for(j in 1:ncol(m)) {
    m[i,j] = max(m[i,j], 1)
  }
}


# plot

jf=2
pdf(file="kleb_indels.pdf",width=6,height=12)

plot(NULL, log="y",
     col=colours[1],pch=points[1], #type="b",
     main=title, xlab="Indel size (bp)", ylab="Count (log)",
     xlim=c(-lim, lim), ylim=c(1, max(m)))

for(i in 1:length(names)) {
  jitter_x = jitter(x,factor=jf)
  points(jitter_x, m[,i+1],
         pch=points[i], #type="b",
         col=line_cols[i])
  points(jitter_x, m[,i+1],
         pch=points[i], type="l",
         col=line_cols[i])
}
legend("topright", names, col=line_cols, pch=points)

dev.off()

# dm <- data.frame(m)
# p <- ggplot(dm, aes(x=dist, y=Platypus)) + geom_line()
